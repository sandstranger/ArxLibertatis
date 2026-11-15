/*
 * Copyright 2011-2022 Arx Libertatis Team (see the AUTHORS file)
 *
 * This file is part of Arx Libertatis.
 *
 * Arx Libertatis is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Arx Libertatis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Arx Libertatis.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "graphics/opengl/GLTexture.h"

#include "graphics/Math.h"
#include "graphics/opengl/GLTextureStage.h"
#include "graphics/opengl/OpenGLRenderer.h"
#include "graphics/opengl/OpenGLUtil.h"
#include "io/fs/FilePath.h"
#include "io/log/Logger.h"

#include <vector>
#include <string>
#include <mutex>
#include <new>
#include <cstring>

#include "TextureCache.h"
#include "Decode.hpp"
#include "ProcessRGB.hpp"
#include "SDL_log.h"

#ifdef ANDROID
#include <android/log.h>
#endif

#ifdef ANDROID
extern bool g_useGLES2_0;
#else
static constexpr bool g_useGLES2_0 = false;
#endif

// Scratch memory of the ETC2 transcode, guarded by s_textureBuffersMutex. It grows to the largest
// texture being converted. It used to reserve 80 MB at the first upload and keep it for good, a good
// part of what a 32 bit process has; now it goes back to the system when the uploads stop for a
// while (trimTextureUploadMemoryIfIdle) and on low memory (releaseTextureUploadMemory).
static std::vector<uint8_t> s_decodeBuffer;   // the base level, BGRA padded to whole blocks
static std::vector<uint8_t> s_mipRgbaBuffer;  // the next mip level
static std::vector<uint8_t> s_etc2Buffer;     // the compressed mip chain, compressed or from a cache
static std::mutex s_textureBuffersMutex;
// Set while this thread uses the buffers: the out of memory handler runs in the thread whose
// allocation failed and must not free them under the upload.
static thread_local bool s_usingTextureBuffers = false;
// Frames since the last ETC2 upload, render thread only.
static unsigned s_framesSinceTextureUpload = 0;
static bool s_enableETC2TextureSupport = true;

extern "C" {

__attribute__((used))
__attribute__((visibility("default")))
void setTextureData(
        const bool enableEtc2TextureSupport,
        const char *pathToTextureCacheDir) {

    s_enableETC2TextureSupport =
            enableEtc2TextureSupport;

    TextureCache::Instance().Init(
            pathToTextureCacheDir,
            30ULL * 1024ULL * 1024ULL * 1024ULL
    );
}

}

namespace {

struct TextureBuffersUser {
    TextureBuffersUser() { s_usingTextureBuffers = true; }
    ~TextureBuffersUser() { s_usingTextureBuffers = false; }
};

} // anonymous namespace

static size_t FreeTextureBuffersLocked() {

    const size_t bytes = s_decodeBuffer.capacity() +
                         s_mipRgbaBuffer.capacity() +
                         s_etc2Buffer.capacity();

    // swap: clear() and resize(0) keep the capacity
    std::vector<uint8_t>().swap(s_decodeBuffer);
    std::vector<uint8_t>().swap(s_mipRgbaBuffer);
    std::vector<uint8_t>().swap(s_etc2Buffer);

    return bytes;
}

size_t releaseTextureUploadMemory() {

    size_t freed = 0;

    if (!s_usingTextureBuffers) {
        std::unique_lock<std::mutex> lock(s_textureBuffersMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            freed += FreeTextureBuffersLocked();
        }
    }

    freed += TextureCache::Instance().TryClearRamCache();

    return freed;
}

void trimTextureUploadMemoryIfIdle() {

    // 120 frames (2-4 seconds) without a texture upload: the level is loaded, the scratch memory goes
    // back to the system. A texture loaded later grows it again, only as large as that texture needs.
    const unsigned IDLE_FRAMES = 120;

    if (s_framesSinceTextureUpload > IDLE_FRAMES) {
        return;
    }

    if (++s_framesSinceTextureUpload <= IDLE_FRAMES) {
        return;
    }

    std::unique_lock<std::mutex> lock(s_textureBuffersMutex, std::try_to_lock);
    if (lock.owns_lock()) {
        FreeTextureBuffersLocked();
    }
}

// A failed operator new gives back what the texture caches hold and tries again; when there is
// nothing left to give, std::bad_alloc as without a handler. The handler must not allocate.
static void TextureMemoryNewHandler() {

    const size_t freed = releaseTextureUploadMemory();

#ifdef ANDROID
    SDL_Log( "Out of memory: %zu bytes of texture caches freed",freed);
#endif

    if (freed == 0) {
        throw std::bad_alloc();
    }
}

void installTextureMemoryNewHandler() {
    std::set_new_handler(TextureMemoryNewHandler);
}

// Exactly the size needed, without copying what the buffer had: resize() grows a vector to at
// least twice its capacity (16 MB for a 9 MB texture after an 8 MB one) and copies the old contents.
static uint8_t *EnsureBuffer(std::vector<uint8_t> &buffer, size_t size) {

    if (buffer.size() < size) {

        if (buffer.capacity() < size) {
            std::vector<uint8_t>().swap(buffer);
        }

        buffer.resize(size);
    }

    return buffer.data();
}

static inline size_t BytesPerPixel(Image::Format format) {

    switch (format) {
        case Image::Format_R8G8B8A8:
        case Image::Format_B8G8R8A8:
            return 4;
        case Image::Format_R8G8B8:
        case Image::Format_B8G8R8:
            return 3;
        case Image::Format_L8A8:
            return 2;
        case Image::Format_L8:
        case Image::Format_A8:
            return 1;
        default:
            return 0;
    }
}

// Every pixel opaque: the image takes ETC2 RGB8, 4 bits per pixel, instead of RGBA8 EAC with 8, whose
// EAC half would only say 255 everywhere. The colors are the same, it is the same ETC2 color block.
static bool IsOpaque(const unsigned char *src, Image::Format format, size_t pixelCount) {

    size_t stride;
    size_t offset;

    switch (format) {
        case Image::Format_R8G8B8:
        case Image::Format_B8G8R8:
        case Image::Format_L8:
            return true;
        case Image::Format_R8G8B8A8:
        case Image::Format_B8G8R8A8:
            stride = 4;
            offset = 3;
            break;
        case Image::Format_L8A8:
            stride = 2;
            offset = 1;
            break;
        case Image::Format_A8:
            stride = 1;
            offset = 0;
            break;
        default:
            return false;
    }

    const unsigned char *alpha = src + offset;
    unsigned char all = 255;

    for (size_t i = 0; i < pixelCount; ++i) {
        all &= alpha[i * stride];
        if ((i & 1023) == 1023 && all != 255) {
            return false;
        }
    }

    return all == 255;
}

// To BGRA, the byte order etcpak reads. The mip levels stay in that order down to the last one (the
// box filter treats the channels alike): the two red/blue swaps over every level before and after its
// compression are gone.
static inline bool ConvertRawToBgra(
        const unsigned char *src,
        uint8_t *dst,
        Image::Format format,
        int width,
        int height,
        int dstStridePixels) {

    const size_t strideBytes = static_cast<size_t>(dstStridePixels) * 4;
    const size_t srcPixelBytes = BytesPerPixel(format);
    const size_t srcStrideBytes = static_cast<size_t>(width) * srcPixelBytes;

    if (srcPixelBytes == 0) {
        return false;
    }

    for (int y = 0; y < height; ++y) {

        const unsigned char *s = src + static_cast<size_t>(y) * srcStrideBytes;
        uint8_t *d = dst + static_cast<size_t>(y) * strideBytes;

        switch (format) {

            case Image::Format_B8G8R8A8:
                memcpy(d, s, static_cast<size_t>(width) * 4);
                break;

            case Image::Format_R8G8B8A8:
                for (int x = 0; x < width; ++x, s += 4, d += 4) {
                    d[0] = s[2];
                    d[1] = s[1];
                    d[2] = s[0];
                    d[3] = s[3];
                }
                break;

            case Image::Format_R8G8B8:
                for (int x = 0; x < width; ++x, s += 3, d += 4) {
                    d[0] = s[2];
                    d[1] = s[1];
                    d[2] = s[0];
                    d[3] = 255;
                }
                break;

            case Image::Format_B8G8R8:
                for (int x = 0; x < width; ++x, s += 3, d += 4) {
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                    d[3] = 255;
                }
                break;

            case Image::Format_L8:
                for (int x = 0; x < width; ++x, s += 1, d += 4) {
                    d[0] = s[0];
                    d[1] = s[0];
                    d[2] = s[0];
                    d[3] = 255;
                }
                break;

            case Image::Format_A8:
                for (int x = 0; x < width; ++x, s += 1, d += 4) {
                    d[0] = 255;
                    d[1] = 255;
                    d[2] = 255;
                    d[3] = s[0];
                }
                break;

            case Image::Format_L8A8:
                for (int x = 0; x < width; ++x, s += 2, d += 4) {
                    d[0] = s[0];
                    d[1] = s[0];
                    d[2] = s[0];
                    d[3] = s[1];
                }
                break;

            default:
                return false;
        }
    }

    return true;
}

// The padding up to whole 4x4 blocks repeats the edge pixels. The base level had zeros there: the
// compressor spent the few colors of the edge blocks on black texels that are never shown.
static void PadToBlocks(uint8_t *pixels, int logicalW, int logicalH, int paddedW, int paddedH) {

    const size_t strideBytes = static_cast<size_t>(paddedW) * 4;

    if (paddedW > logicalW) {
        for (int y = 0; y < logicalH; ++y) {
            uint8_t *row = pixels + static_cast<size_t>(y) * strideBytes;
            const uint8_t *last = row + static_cast<size_t>(logicalW - 1) * 4;
            for (int x = logicalW; x < paddedW; ++x) {
                memcpy(row + static_cast<size_t>(x) * 4, last, 4);
            }
        }
    }

    const uint8_t *lastRow = pixels + static_cast<size_t>(logicalH - 1) * strideBytes;
    for (int y = logicalH; y < paddedH; ++y) {
        memcpy(pixels + static_cast<size_t>(y) * strideBytes, lastRow, strideBytes);
    }
}

static inline int Align4(int x) {
    return (x + 3) & ~3;
}

struct Etc2MipLevel {

    int logicalW;
    int logicalH;

    int paddedW;
    int paddedH;

    size_t offset;
    size_t compressedSize;
};

static inline void ComputeMipChain(
        int baseLogicalW,
        int baseLogicalH,
        size_t blockBytes,
        std::vector<Etc2MipLevel> &outLevels) {

    outLevels.clear();

    int logicalW = baseLogicalW;
    int logicalH = baseLogicalH;

    size_t offset = 0;

    for (;;) {

        Etc2MipLevel level;

        level.logicalW = logicalW;
        level.logicalH = logicalH;
        level.paddedW = Align4(logicalW);
        level.paddedH = Align4(logicalH);
        level.offset = offset;
        level.compressedSize = static_cast<size_t>(level.paddedW / 4) *
                               static_cast<size_t>(level.paddedH / 4) *
                               blockBytes;

        outLevels.push_back(level);

        offset += level.compressedSize;

        if (logicalW == 1 && logicalH == 1) {
            break;
        }

        logicalW = (logicalW > 1) ? (logicalW >> 1) : 1;
        logicalH = (logicalH > 1) ? (logicalH >> 1) : 1;
    }
}

static inline void DownsampleAndPad2x2(
        const uint8_t *src,
        int srcLogicalW,
        int srcLogicalH,
        int srcPaddedW,

        uint8_t *dst,
        int dstLogicalW,
        int dstLogicalH,
        int dstPaddedW,
        int dstPaddedH) {

    const size_t srcStrideBytes = static_cast<size_t>(srcPaddedW) * 4;
    const size_t dstStrideBytes = static_cast<size_t>(dstPaddedW) * 4;

    for (int y = 0; y < dstLogicalH; ++y) {

        const int sy0 = y * 2;
        const int sy1 = (sy0 + 1 < srcLogicalH) ? (sy0 + 1) : (srcLogicalH - 1);

        const uint8_t *row0 = src + static_cast<size_t>(sy0) * srcStrideBytes;
        const uint8_t *row1 = src + static_cast<size_t>(sy1) * srcStrideBytes;

        uint8_t *dstRow = dst + static_cast<size_t>(y) * dstStrideBytes;

        for (int x = 0; x < dstLogicalW; ++x) {

            const int sx0 = x * 2;
            const int sx1 = (sx0 + 1 < srcLogicalW) ? (sx0 + 1) : (srcLogicalW - 1);

            const uint8_t *p00 = row0 + static_cast<size_t>(sx0) * 4;
            const uint8_t *p01 = row0 + static_cast<size_t>(sx1) * 4;
            const uint8_t *p10 = row1 + static_cast<size_t>(sx0) * 4;
            const uint8_t *p11 = row1 + static_cast<size_t>(sx1) * 4;

            uint8_t *out = dstRow + static_cast<size_t>(x) * 4;

            for (int c = 0; c < 4; ++c) {
                out[c] = static_cast<uint8_t>(
                        (static_cast<uint32_t>(p00[c]) + static_cast<uint32_t>(p01[c]) +
                         static_cast<uint32_t>(p10[c]) + static_cast<uint32_t>(p11[c]) + 2) >> 2);
            }
        }
    }

    PadToBlocks(dst, dstLogicalW, dstLogicalH, dstPaddedW, dstPaddedH);
}

// Compresses the mip chain into s_etc2Buffer, s_textureBuffersMutex held.
static bool CompressEtc2MipChain(
        const unsigned char *srcData,
        Image::Format format,
        const std::vector<Etc2MipLevel> &levels,
        int numLevels,
        GLenum etc2Format,
        size_t totalCompressedSize) {

    const Etc2MipLevel &base = levels[0];

    const size_t baseSize = static_cast<size_t>(base.paddedW) *
                            static_cast<size_t>(base.paddedH) * 4;

    // Level 1 is the largest one after the base: the two buffers take turns, every later level
    // fits in either of them.
    const size_t nextSize = (numLevels > 1)
            ? static_cast<size_t>(levels[1].paddedW) * static_cast<size_t>(levels[1].paddedH) * 4
            : 0;

    uint8_t *cur = EnsureBuffer(s_decodeBuffer, baseSize);
    uint8_t *nxt = (nextSize > 0) ? EnsureBuffer(s_mipRgbaBuffer, nextSize) : nullptr;
    uint8_t *etc2Blob = EnsureBuffer(s_etc2Buffer, totalCompressedSize);

    if (!ConvertRawToBgra(srcData, cur, format, base.logicalW, base.logicalH, base.paddedW)) {
        return false;
    }

    PadToBlocks(cur, base.logicalW, base.logicalH, base.paddedW, base.paddedH);

    for (int i = 0; i < numLevels; ++i) {

        const Etc2MipLevel &level = levels[i];

        const uint32_t blocks = static_cast<uint32_t>(
                static_cast<size_t>(level.paddedW / 4) * static_cast<size_t>(level.paddedH / 4));

        if (etc2Format == GL_COMPRESSED_RGB8_ETC2) {
            CompressEtc2Rgb(
                    reinterpret_cast<const uint32_t *>(cur),
                    reinterpret_cast<uint64_t *>(etc2Blob + level.offset),
                    blocks,
                    static_cast<uint32_t>(level.paddedW),
                    true
            );
        } else {
            CompressEtc2Rgba(
                    reinterpret_cast<const uint32_t *>(cur),
                    reinterpret_cast<uint64_t *>(etc2Blob + level.offset),
                    blocks,
                    static_cast<uint32_t>(level.paddedW),
                    true
            );
        }

        if (i + 1 >= numLevels) {
            break;
        }

        const Etc2MipLevel &next = levels[i + 1];

        DownsampleAndPad2x2(
                cur, level.logicalW, level.logicalH, level.paddedW,
                nxt, next.logicalW, next.logicalH, next.paddedW, next.paddedH
        );

        std::swap(cur, nxt);
    }

    return true;
}

static void SetTextureSwizzle(GLint r, GLint g, GLint b, GLint a) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, r);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, g);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, b);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, a);
}

/*
 * ETC2 upload.
 *
 * IMPORTANT:
 *
 * This is a free/static function, therefore it cannot access
 * GLTexture::renderer directly.
 *
 * OpenGLRenderer is explicitly passed in from GLTexture::upload().
 */
static bool UploadAsEtc2(
        OpenGLRenderer *renderer,
        GLuint tex,
        int width,
        int height,
        const unsigned char *srcData,
        Image::Format format,
        bool wantMipmaps) {

    if (renderer == nullptr ||
        tex == GL_NONE ||
        srcData == nullptr ||
        width <= 0 ||
        height <= 0) {

        return false;
    }

    const size_t bytesPerPixel = BytesPerPixel(format);

    if (bytesPerPixel == 0) {
        return false;
    }

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t rawSourceSize = pixelCount * bytesPerPixel;

    const GLenum etc2Format = IsOpaque(srcData, format, pixelCount)
            ? GL_COMPRESSED_RGB8_ETC2
            : GL_COMPRESSED_RGBA8_ETC2_EAC;

    const size_t blockBytes = (etc2Format == GL_COMPRESSED_RGB8_ETC2) ? 8 : 16;

    std::vector<Etc2MipLevel> levels;

    ComputeMipChain(width, height, blockBytes, levels);

    const int numLevels = wantMipmaps ? static_cast<int>(levels.size()) : 1;

    size_t totalCompressedSize = 0;

    for (int i = 0; i < numLevels; ++i) {
        totalCompressedSize += levels[i].compressedSize;
    }

    // Computed once for both caches, it reads every byte of the image (64 bit multiplies, slow on
    // ARMv7); it was computed three times per texture. A texture without its mip levels is another
    // entry: with one key for both, the disk cache threw the entry away whenever the other kind of
    // the same image was loaded.
    uint64_t hash = ComputeTextureHash(srcData, rawSourceSize, width, height,
                                       static_cast<int>(etc2Format));

    if (numLevels == 1 && levels.size() > 1) {
        hash ^= 0x9E3779B97F4A7C15ULL;
    }

    std::lock_guard<std::mutex> lock(s_textureBuffersMutex);

    TextureBuffersUser bufferUser;

    s_framesSinceTextureUpload = 0;

    // The caches read into the buffer the compression writes, one buffer less.
    EnsureBuffer(s_etc2Buffer, totalCompressedSize);

    size_t cachedSize = 0;

    bool cacheHit =
            TextureCache::Instance().TryGetFromRamCache(hash, s_etc2Buffer, &cachedSize) &&
            cachedSize == totalCompressedSize;

    if (!cacheHit) {

        cacheHit =
                TextureCache::Instance().TryGetCachedETC2(
                        hash,
                        width,
                        height,
                        static_cast<int>(etc2Format),
                        numLevels,
                        s_etc2Buffer,
                        &cachedSize
                ) &&
                cachedSize == totalCompressedSize;

        if (cacheHit) {

            TextureCache::Instance().SaveToRamCache(
                    hash,
                    s_etc2Buffer.data(),
                    cachedSize,
                    width,
                    height,
                    etc2Format
            );
        }
    }

    if (!cacheHit) {

        if (!CompressEtc2MipChain(srcData, format, levels, numLevels, etc2Format,
                                  totalCompressedSize)) {
            return false;
        }

        TextureCache::Instance().SaveToRamCache(
                hash,
                s_etc2Buffer.data(),
                totalCompressedSize,
                width,
                height,
                etc2Format
        );

        TextureCache::Instance().SaveToCacheAsync(
                hash,
                width,
                height,
                static_cast<int>(etc2Format),
                numLevels,
                s_etc2Buffer.data(),
                totalCompressedSize
        );
    }

    const uint8_t *etc2Blob = s_etc2Buffer.data();

    glBindTexture(
            GL_TEXTURE_2D,
            tex
    );

    glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_BASE_LEVEL,
            0
    );

    glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_MAX_LEVEL,
            numLevels - 1
    );

    for (int i = 0;
         i < numLevels;
         ++i) {

        const Etc2MipLevel &L =
                levels[i];

        glCompressedTexImage2D(
                GL_TEXTURE_2D,
                i,

                etc2Format,

                static_cast<GLsizei>(
                        L.logicalW
                ),

                static_cast<GLsizei>(
                        L.logicalH
                ),

                0,

                static_cast<GLsizei>(
                        L.compressedSize
                ),

                etc2Blob +
                L.offset
        );

        const GLenum error =
                glGetError();

        if (error != GL_NO_ERROR) {

            SDL_Log(
                    "ETC2 mip upload failed: "
                    "level=%d "
                    "logical=%dx%d "
                    "padded=%dx%d "
                    "compressedSize=%zu "
                    "error=0x%04X\n",

                    i,

                    L.logicalW,
                    L.logicalH,

                    L.paddedW,
                    L.paddedH,

                    L.compressedSize,

                    static_cast<unsigned>(
                            error
                    )
            );

            return false;
        }
    }

    // the texture object may have had a one or two channel image before
    SetTextureSwizzle(GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA);

    glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_MIN_FILTER,
            numLevels > 1
            ? GL_LINEAR_MIPMAP_LINEAR
            : GL_LINEAR
    );

    glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_MAG_FILTER,
            GL_LINEAR
    );

    if (numLevels > 1 &&
        renderer->getMaxAnisotropy() > 1.0f) {

        glTexParameterf(
                GL_TEXTURE_2D,
                GL_TEXTURE_MAX_ANISOTROPY_EXT,
                renderer->getMaxAnisotropy()
        );
    }

    return true;
}

GLTexture::GLTexture(
        OpenGLRenderer *_renderer)

        : renderer(_renderer),
          tex(GL_NONE),
          wrapMode(TextureStage::WrapRepeat),
          minFilter(TextureStage::FilterLinear),
          magFilter(TextureStage::FilterNearest),
          isNPOT(false) {
}

GLTexture::~GLTexture() {
    destroy();
}

bool GLTexture::create() {

    arx_assert_msg(
            tex == GL_NONE,
            "leaking OpenGL texture"
    );

    glGenTextures(
            1,
            &tex
    );

    wrapMode =
            TextureStage::WrapRepeat;

    minFilter =
            TextureStage::FilterNearest;

    magFilter =
            TextureStage::FilterLinear;

    Vec2i nextPowerOfTwo(
            GetNextPowerOf2(
                    unsigned(
                            getSize().x
                    )
            ),

            GetNextPowerOf2(
                    unsigned(
                            getSize().y
                    )
            )
    );

    m_storedSize =
            renderer->hasTextureNPOT()
            ? getSize()
            : nextPowerOfTwo;

    isNPOT =
            (getSize() != nextPowerOfTwo);

    return (
            tex != GL_NONE
    );
}

void GLTexture::upload() {

    arx_assert(
            tex != GL_NONE
    );

    glBindTexture(
            GL_TEXTURE_2D,
            tex
    );

    renderer->GetTextureStage(0)->current =
            this;

    if (!renderer->hasIntensityTextures() &&
        isIntensity()) {

        arx_assert(
                getFormat() ==
                Image::Format_L8
        );

        Image converted;

        // Convert only the decoded image dimensions here. The upload paths
        // extend it to the stored size later when NPOT padding is required.
        converted.create(
                m_image.getWidth(),
                m_image.getHeight(),
                Image::Format_L8A8
        );

        unsigned char *input =
                m_image.getData();

        unsigned char *end =
                input +
                m_image.getWidth() *
                m_image.getHeight();

        unsigned char *output =
                converted.getData();

        for (; input != end; ++input) {

            *output++ = *input;
            *output++ = *input;
        }

        m_image =
                converted;

        m_format =
                Image::Format_L8A8;

        m_flags &=
                ~Intensity;
    }

    if (!renderer->hasBGRTextureTransfer() &&
        (
                getFormat() ==
                Image::Format_B8G8R8 ||

                getFormat() ==
                Image::Format_B8G8R8A8
        )) {

        Image::Format rgbFormat =
                getFormat() ==
                Image::Format_B8G8R8

                ? Image::Format_R8G8B8

                : Image::Format_R8G8B8A8;

        m_image.convertTo(
                rgbFormat
        );

        m_format =
                rgbFormat;
    }

    if (getStoredSize() != getSize()) {

        m_flags &=
                ~HasMipmaps;
    }

    const bool isSupportedForEtc2 =
            (
                    m_format ==
                    Image::Format_R8G8B8A8 ||

                    m_format ==
                    Image::Format_B8G8R8A8 ||

                    m_format ==
                    Image::Format_R8G8B8 ||

                    m_format ==
                    Image::Format_B8G8R8 ||

                    m_format ==
                    Image::Format_L8 ||

                    m_format ==
                    Image::Format_A8 ||

                    m_format ==
                    Image::Format_L8A8
            );

    // ETC2 only for textures loaded from files. Textures built at runtime (font glyph
    // atlases, procedural images) change while the game runs: compressing them on the
    // CPU again for every update costs time, fills the disk cache with one entry per
    // version, and EAC artifacts degrade small text.
    if (!g_useGLES2_0 &&
        s_enableETC2TextureSupport &&
        isSupportedForEtc2 &&
        !getFileName().empty() &&
        m_image.isValid()) {

        const int storedW =
                static_cast<int>(
                        getStoredSize().x
                );

        const int storedH =
                static_cast<int>(
                        getStoredSize().y
                );

        const unsigned char *srcData =
                m_image.getData();

        Image extended;

        // Pad to the renderer's actual stored size only. UploadAsEtc2 pads
        // each compressed block internally; passing block-aligned dimensions
        // here would incorrectly change the texture's logical size on NPOT
        // GLES 3 textures.
        if (getStoredSize() != getSize()) {

            extended.create(
                    static_cast<size_t>(storedW),
                    static_cast<size_t>(storedH),
                    m_image.getFormat()
            );

            extended.extendClampToEdgeBorder(
                    m_image
            );

            srcData =
                    extended.getData();
        }

        const bool wantsMip =
                hasMipmaps();

        if (UploadAsEtc2(
                renderer,
                tex,
                storedW,
                storedH,
                srcData,
                m_format,
                wantsMip)) {

            glTexParameteri(
                    GL_TEXTURE_2D,
                    GL_TEXTURE_MIN_FILTER,
                    wantsMip
                    ? GL_LINEAR_MIPMAP_LINEAR
                    : GL_LINEAR
            );

            glTexParameteri(
                    GL_TEXTURE_2D,
                    GL_TEXTURE_MAG_FILTER,
                    GL_LINEAR
            );

            return;
        }
    }

#ifdef ANDROID
    // GLES has no BGR transfer and no intensity textures: by now B8G8R8(A8) images are R8G8B8(A8)
    // and intensity ones L8A8, see above. RGBA and RGB images go up as they are, every image was
    // copied to RGBA before. On GLES 3 one and two channel images go to GL_R8 / GL_RG8 and the
    // swizzle gives what the RGBA copy had: the font pages (A8, uploaded again for new glyphs) take
    // a quarter of the memory and no copy.
    Image glesImage;
    const Image *uploadImage = &m_image;
    GLint internal = GL_NONE;
    GLenum format = GL_NONE;
    GLint swizzle[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
    const Image::Format sourceFormat = m_image.getFormat();

    if (!isIntensity() && sourceFormat == Image::Format_R8G8B8A8) {
        internal = g_useGLES2_0 ? GL_RGBA : GL_RGBA8;
        format = GL_RGBA;
    } else if (!isIntensity() && sourceFormat == Image::Format_R8G8B8) {
        internal = g_useGLES2_0 ? GL_RGB : GL_RGB8;
        format = GL_RGB;
    } else if (!g_useGLES2_0 && !isIntensity() && sourceFormat == Image::Format_L8A8) {
        internal = GL_RG8;
        format = GL_RG;
        swizzle[0] = swizzle[1] = swizzle[2] = GL_RED;
        swizzle[3] = GL_GREEN;
    } else if (!g_useGLES2_0 && !isIntensity() && sourceFormat == Image::Format_L8) {
        internal = GL_R8;
        format = GL_RED;
        swizzle[0] = swizzle[1] = swizzle[2] = GL_RED;
        swizzle[3] = GL_ONE;
    } else if (!g_useGLES2_0 && !isIntensity() && sourceFormat == Image::Format_A8) {
        internal = GL_R8;
        format = GL_RED;
        swizzle[0] = swizzle[1] = swizzle[2] = GL_ONE;
        swizzle[3] = GL_RED;
    } else {
        // OpenGL ES 2.0 feature level: the RGBA copy as before.
        glesImage.create(m_image.getWidth(), m_image.getHeight(), Image::Format_R8G8B8A8);
        const unsigned char * source = m_image.getData();
        unsigned char * destination = glesImage.getData();
        const size_t pixelCount = m_image.getWidth() * m_image.getHeight();

        for(size_t i = 0; i < pixelCount; ++i) {
            const unsigned char * src = source;
            unsigned char * dst = destination;
            if(isIntensity()) {
                const unsigned char intensity = src[0];
                dst[0] = intensity;
                dst[1] = intensity;
                dst[2] = intensity;
                dst[3] = intensity;
            } else switch(sourceFormat) {
                case Image::Format_L8: {
                    const unsigned char luminance = src[0];
                    dst[0] = luminance;
                    dst[1] = luminance;
                    dst[2] = luminance;
                    dst[3] = 255;
                    break;
                }
                case Image::Format_A8:
                    dst[0] = dst[1] = dst[2] = 255;
                    dst[3] = src[0];
                    break;
                case Image::Format_L8A8: {
                    const unsigned char luminance = src[0];
                    dst[0] = luminance;
                    dst[1] = luminance;
                    dst[2] = luminance;
                    dst[3] = src[1];
                    break;
                }
                case Image::Format_R8G8B8:
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = 255;
                    break;
                case Image::Format_B8G8R8:
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                    dst[3] = 255;
                    break;
                case Image::Format_R8G8B8A8:
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = src[3];
                    break;
                case Image::Format_B8G8R8A8:
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                    dst[3] = src[3];
                    break;
                default:
                    LogError << "Unsupported source texture format for OpenGL ES";
                    return;
            }
            source += m_image.getNumChannels();
            destination += 4;
        }

        uploadImage = &glesImage;
        internal = g_useGLES2_0 ? GL_RGBA : GL_RGBA8;
        format = GL_RGBA;
    }

    if (!g_useGLES2_0) {
        // also back to the identity when the texture object had a one or two channel image
        SetTextureSwizzle(swizzle[0], swizzle[1], swizzle[2], swizzle[3]);
    }

    const Image &textureImage = *uploadImage;
#else
    GLint internalUnsized;
    GLint internalSized;
    GLenum format;

    if (isIntensity()) {
        internalUnsized = GL_INTENSITY;
        internalSized = GL_INTENSITY8;
        format = GL_RED;
    } else if (getFormat() == Image::Format_L8) {
        internalUnsized = GL_LUMINANCE;
        internalSized = GL_LUMINANCE8;
        format = GL_LUMINANCE;
    } else if (getFormat() == Image::Format_A8) {
        internalUnsized = GL_ALPHA;
        internalSized = GL_ALPHA8;
        format = GL_ALPHA;
    } else if (getFormat() == Image::Format_L8A8) {
        internalUnsized = GL_LUMINANCE_ALPHA;
        internalSized = GL_LUMINANCE8_ALPHA8;
        format = GL_LUMINANCE_ALPHA;
    } else if (getFormat() == Image::Format_R8G8B8) {
        internalUnsized = GL_RGB;
        internalSized = GL_RGB8;
        format = GL_RGB;
    } else if (getFormat() == Image::Format_B8G8R8) {
        internalUnsized = GL_RGB;
        internalSized = GL_RGB8;
        format = GL_BGR;
    } else if (getFormat() == Image::Format_R8G8B8A8) {
        internalUnsized = GL_RGBA;
        internalSized = GL_RGBA8;
        format = GL_RGBA;
    } else if (getFormat() == Image::Format_B8G8R8A8) {
        internalUnsized = GL_RGBA;
        internalSized = GL_RGBA8;
        format = GL_BGRA;
    } else {
        arx_assert_msg(false, "Unsupported image format: %ld", long(getFormat()));
        return;
    }

    GLint internal = renderer->hasSizedTextureFormats() ? internalSized : internalUnsized;
    const Image &textureImage = m_image;
#endif

    const bool generateMipmaps = hasMipmaps();
    if(generateMipmaps) {
#ifndef ANDROID
        glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
#endif
        if(renderer->getMaxAnisotropy() > 1.f) {
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                            renderer->getMaxAnisotropy());
        }
    } else {
#ifndef ANDROID
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
#else
        if(!g_useGLES2_0) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        }
#endif
    }

    if (getStoredSize() != getSize()) {

        Image extended;

        extended.create(
                size_t(getStoredSize().x),
                size_t(getStoredSize().y),
                textureImage.getFormat()
        );

        extended.extendClampToEdgeBorder(textureImage);

        glTexImage2D(
                GL_TEXTURE_2D,
                0,
                internal,
                getStoredSize().x,
                getStoredSize().y,
                0,
                format,
                GL_UNSIGNED_BYTE,
                extended.getData()
        );

    } else {

        glTexImage2D(
                GL_TEXTURE_2D,
                0,
                internal,
                getSize().x,
                getSize().y,
                0,
                format,
                GL_UNSIGNED_BYTE,
                textureImage.getData()
        );
    }

#ifdef ANDROID
    if(generateMipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
#endif
}

void GLTexture::destroy() {

    if (tex) {

        glDeleteTextures(
                1,
                &tex
        );

        tex =
                GL_NONE;
    }

    for (size_t i = 0;
         i < renderer->getTextureStageCount();
         ++i) {

        GLTextureStage *stage =
                renderer->GetTextureStage(i);

        if (stage->tex == this) {
            stage->tex = nullptr;
        }

        if (stage->current == this) {
            stage->current = nullptr;
        }
    }
}

static const GLint arxToGlWrapMode[] = {

        GL_REPEAT,
        GL_MIRRORED_REPEAT,
        GL_CLAMP_TO_EDGE
};

static const GLint arxToGlFilter[][2] = {

        {
                GL_NEAREST,
                GL_LINEAR
        },

        {
                GL_NEAREST_MIPMAP_LINEAR,
                GL_LINEAR_MIPMAP_LINEAR
        }
};

void GLTexture::apply(
        GLTextureStage *stage) {

    arx_assert(
            stage != nullptr
    );

    arx_assert(
            stage->tex == this
    );

#ifdef ANDROID
    // GLES 3.0 and GLES 2.0 with OES_texture_npot support repeat and mirror
    // wrapping for NPOT textures. Keep the clamp fallback only when the
    // renderer had to pad the texture to a power-of-two size.
    const bool canWrapNPOT = renderer->hasTextureNPOT();
#else
    const bool canWrapNPOT = false;
#endif

    TextureStage::WrapMode newWrapMode =
            (!isNPOT || canWrapNPOT)
            ? stage->getWrapMode()
            : TextureStage::WrapClamp;

    if (newWrapMode != wrapMode) {

        wrapMode =
                newWrapMode;

        GLint glwrap =
                arxToGlWrapMode[
                        wrapMode
                ];

        glTexParameteri(
                GL_TEXTURE_2D,
                GL_TEXTURE_WRAP_T,
                glwrap
        );

        glTexParameteri(
                GL_TEXTURE_2D,
                GL_TEXTURE_WRAP_S,
                glwrap
        );
    }

    if (stage->getMinFilter() != minFilter) {

        minFilter =
                stage->getMinFilter();

        const int mipFilter =
                hasMipmaps()
                ? 1
                : 0;

        glTexParameteri(
                GL_TEXTURE_2D,
                GL_TEXTURE_MIN_FILTER,
                arxToGlFilter[
                        mipFilter
                ][
                        minFilter
                ]
        );
    }

    if (stage->getMagFilter() != magFilter) {

        magFilter =
                stage->getMagFilter();

        glTexParameteri(
                GL_TEXTURE_2D,
                GL_TEXTURE_MAG_FILTER,
                arxToGlFilter[
                        0
                ][
                        magFilter
                ]
        );
    }
}

void GLTexture::updateMaxAnisotropy() {

    if (hasMipmaps()) {

        glBindTexture(
                GL_TEXTURE_2D,
                tex
        );

        renderer->GetTextureStage(0)->current =
                this;

        glTexParameterf(
                GL_TEXTURE_2D,
                GL_TEXTURE_MAX_ANISOTROPY_EXT,
                renderer->getMaxAnisotropy()
        );
    }
}
