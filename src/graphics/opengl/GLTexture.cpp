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
#include <vector>
#include <string>
#include <mutex>
#include <cstring>
#include "TextureCache.h"
#include "Decode.hpp"
#include "ProcessRGB.hpp"

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
#include "arm_neon.h"
#endif

extern bool g_useGLES2_0;
static std::vector<uint8_t> s_decodeBuffer;
static std::vector<uint8_t> s_etc2Buffer;
static std::vector<uint8_t> s_etc2CacheBuffer;
static std::mutex s_textureBuffersMutex;
static bool s_textureBuffersInited = false;
static bool g_EnableETC2TextureSupport = true;

extern "C"{
    __attribute__((used)) __attribute__((visibility("default")))    
    void setTextureData(const bool enableETC2TextureSupport, const char* pathToTextureCacheDir){
        g_EnableETC2TextureSupport = enableETC2TextureSupport;
        TextureCache::Instance().Init(pathToTextureCacheDir,30ULL * 1024ULL * 1024ULL * 1024ULL);        
    }
}

static inline void EnsureTextureBuffersInited()
{
    if (!s_textureBuffersInited)
    {
        std::lock_guard<std::mutex> lock(s_textureBuffersMutex);
        if (!s_textureBuffersInited)
        {
            s_decodeBuffer.reserve(16 * 1024 * 1024);
            s_etc2Buffer.reserve(8 * 1024 * 1024);
            s_etc2CacheBuffer.reserve(8 * 1024 * 1024);
            s_textureBuffersInited = true;
        }
    }
}

static inline void SwapRB_NEON(uint8_t* dpic, size_t pixelCount) {
#if defined(__aarch64__) || defined(_M_ARM64)
    size_t i = 0;
    for (; i <= pixelCount - 16; i += 16) {
        uint8x16x4_t pixels = vld4q_u8(&dpic[i * 4]);
        uint8x16_t temp = pixels.val[0];
        pixels.val[0] = pixels.val[2];
        pixels.val[2] = temp;
        vst4q_u8(&dpic[i * 4], pixels);
    }
    for (; i < pixelCount; i++) {
        std::swap(dpic[i * 4], dpic[i * 4 + 2]);
    }
#else
    for (size_t i = 0; i < pixelCount; i++) {
        std::swap(dpic[i * 4], dpic[i * 4 + 2]);
    }
#endif
}


static inline bool ConvertRawToRgba(const unsigned char* src, uint8_t* dst,
                                    Image::Format format,
                                    int width, int height, int dstStridePixels)
{
    const size_t strideBytes = static_cast<size_t>(dstStridePixels) * 4;
    
    switch (format) {
        case Image::Format_R8G8B8A8:
            for (int y = 0; y < height; ++y) {
                memcpy(dst + y * strideBytes,
                       src + static_cast<size_t>(y) * width * 4,
                       static_cast<size_t>(width) * 4);
            }
            break;
            
        case Image::Format_B8G8R8A8:
            for (int y = 0; y < height; ++y) {
                const unsigned char* srow = src + static_cast<size_t>(y) * width * 4;
                uint8_t* drow = dst + y * strideBytes;
                for (int x = 0; x < width; ++x) {
                    drow[x * 4 + 0] = srow[x * 4 + 2];
                    drow[x * 4 + 1] = srow[x * 4 + 1];
                    drow[x * 4 + 2] = srow[x * 4 + 0];
                    drow[x * 4 + 3] = srow[x * 4 + 3];
                }
            }
            break;
            
        case Image::Format_R8G8B8:
            for (int y = 0; y < height; ++y) {
                const unsigned char* srow = src + static_cast<size_t>(y) * width * 3;
                uint8_t* drow = dst + y * strideBytes;
                for (int x = 0; x < width; ++x) {
                    drow[x * 4 + 0] = srow[x * 3 + 0];
                    drow[x * 4 + 1] = srow[x * 3 + 1];
                    drow[x * 4 + 2] = srow[x * 3 + 2];
                    drow[x * 4 + 3] = 255;
                }
            }
            break;
            
        case Image::Format_B8G8R8:
            for (int y = 0; y < height; ++y) {
                const unsigned char* srow = src + static_cast<size_t>(y) * width * 3;
                uint8_t* drow = dst + y * strideBytes;
                for (int x = 0; x < width; ++x) {
                    drow[x * 4 + 0] = srow[x * 3 + 2];
                    drow[x * 4 + 1] = srow[x * 3 + 1];
                    drow[x * 4 + 2] = srow[x * 3 + 0];
                    drow[x * 4 + 3] = 255;
                }
            }
            break;
            
        case Image::Format_L8:
            for (int y = 0; y < height; ++y) {
                const unsigned char* srow = src + static_cast<size_t>(y) * width;
                uint8_t* drow = dst + y * strideBytes;
                for (int x = 0; x < width; ++x) {
                    drow[x * 4 + 0] = srow[x];
                    drow[x * 4 + 1] = srow[x];
                    drow[x * 4 + 2] = srow[x];
                    drow[x * 4 + 3] = 255;
                }
            }
            break;
            
        case Image::Format_A8:
            for (int y = 0; y < height; ++y) {
                const unsigned char* srow = src + static_cast<size_t>(y) * width;
                uint8_t* drow = dst + y * strideBytes;
                for (int x = 0; x < width; ++x) {
                    drow[x * 4 + 0] = 255;
                    drow[x * 4 + 1] = 255;
                    drow[x * 4 + 2] = 255;
                    drow[x * 4 + 3] = srow[x];
                }
            }
            break;
            
        case Image::Format_L8A8:
            for (int y = 0; y < height; ++y) {
                const unsigned char* srow = src + static_cast<size_t>(y) * width * 2;
                uint8_t* drow = dst + y * strideBytes;
                for (int x = 0; x < width; ++x) {
                    drow[x * 4 + 0] = srow[x * 2 + 0];
                    drow[x * 4 + 1] = srow[x * 2 + 0];
                    drow[x * 4 + 2] = srow[x * 2 + 0];
                    drow[x * 4 + 3] = srow[x * 2 + 1];
                }
            }
            break;
            
        default:
            return false;
    }
    return true;
}


static bool UploadAsEtc2(GLuint tex, int width, int height,
                         const unsigned char* srcData, Image::Format format,
                         const char* textureName)
{
    EnsureTextureBuffersInited();
    
    std::lock_guard<std::mutex> lock(s_textureBuffersMutex);
    
    
    const size_t paddedWidth  = ((static_cast<size_t>(width) + 3) / 4) * 4;
    const size_t paddedHeight = ((static_cast<size_t>(height) + 3) / 4) * 4;
    const size_t blocks = (paddedWidth / 4) * (paddedHeight / 4);
    const size_t etc2CompressedSize = blocks * 16;
    const size_t decodeSize = paddedWidth * paddedHeight * 4;
    
    size_t rawSourceSize = 0;
    switch (format) {
        case Image::Format_R8G8B8A8:
        case Image::Format_B8G8R8A8:
            rawSourceSize = static_cast<size_t>(width) * height * 4;
            break;
        case Image::Format_R8G8B8:
        case Image::Format_B8G8R8:
            rawSourceSize = static_cast<size_t>(width) * height * 3;
            break;
        case Image::Format_L8:
        case Image::Format_A8:
            rawSourceSize = static_cast<size_t>(width) * height;
            break;
        case Image::Format_L8A8:
            rawSourceSize = static_cast<size_t>(width) * height * 2;
            break;
        default:
            rawSourceSize = static_cast<size_t>(width) * height * 4;
            break;
    }
    
    const uint64_t hash = ComputeTextureHash(srcData, rawSourceSize, width, height,GL_COMPRESSED_RGBA8_ETC2_EAC);
    
    bool cacheHit = false;
    uint8_t* etc2Data = nullptr;
    size_t cachedSize = 0;
    
    cacheHit = TextureCache::Instance().TryGetFromRamCache(hash, s_etc2CacheBuffer, &cachedSize);
        
    if (!cacheHit) {
        cacheHit = TextureCache::Instance().TryGetCachedETC2(
                textureName ? textureName : "",
                srcData, rawSourceSize,
                width, height,
                GL_COMPRESSED_RGBA8_ETC2_EAC,
                1,
                s_etc2CacheBuffer, &cachedSize
            );
            
        if (cacheHit) {
            TextureCache::Instance().SaveToRamCache(hash,reinterpret_cast<std::byte*>(s_etc2CacheBuffer.data()),
                    cachedSize, width, height, GL_COMPRESSED_RGBA8_ETC2_EAC);
            }
        }
             
    if (cacheHit) {
        etc2Data = reinterpret_cast<uint8_t*>(s_etc2CacheBuffer.data());
    }
    
    if (!cacheHit) {
        if (s_decodeBuffer.size() < decodeSize) {
            s_decodeBuffer.resize(decodeSize);
        }
        uint8_t* rgbaBuffer = s_decodeBuffer.data();
        memset(rgbaBuffer, 0, decodeSize);
        
        if (!ConvertRawToRgba(srcData, rgbaBuffer, format, width, height,
                              static_cast<int>(paddedWidth))) {
            return false;
        }
        
        SwapRB_NEON(rgbaBuffer, paddedWidth * paddedHeight);
        
        if (s_etc2Buffer.size() < etc2CompressedSize) {
            s_etc2Buffer.resize(etc2CompressedSize);
        }
        etc2Data = s_etc2Buffer.data();
        
        CompressEtc2Rgba(
            reinterpret_cast<const uint32_t*>(rgbaBuffer),
            reinterpret_cast<uint64_t*>(etc2Data),
            blocks,
            paddedWidth,
            true
        );

        TextureCache::Instance().SaveToRamCache(hash, etc2Data, etc2CompressedSize,
                                                width, height, GL_COMPRESSED_RGBA8_ETC2_EAC);
        TextureCache::Instance().SaveToCacheAsync(
                textureName ? textureName : "",
                srcData, rawSourceSize,
                width, height,
                GL_COMPRESSED_RGBA8_ETC2_EAC,
                1,
                etc2Data, etc2CompressedSize
        );
    }
    
    glBindTexture(GL_TEXTURE_2D, tex);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA8_ETC2_EAC,
                           static_cast<GLsizei>(paddedWidth),
                           static_cast<GLsizei>(paddedHeight),
                           0,
                           static_cast<GLsizei>(etc2CompressedSize),
                           etc2Data);
    
    return (glGetError() == GL_NO_ERROR);
}

GLTexture::GLTexture(OpenGLRenderer * _renderer)
	: renderer(_renderer)
	, tex(GL_NONE)
	, wrapMode(TextureStage::WrapRepeat)
	, minFilter(TextureStage::FilterLinear)
	, magFilter(TextureStage::FilterNearest)
	, isNPOT(false)
{ }

GLTexture::~GLTexture() {
	destroy();
}

bool GLTexture::create() {
	
	arx_assert_msg(tex == GL_NONE, "leaking OpenGL texture");
	
	glGenTextures(1, &tex);
	
	wrapMode = TextureStage::WrapRepeat;
	minFilter = TextureStage::FilterNearest;
	magFilter = TextureStage::FilterLinear;
	
	Vec2i nextPowerOfTwo(GetNextPowerOf2(unsigned(getSize().x)), GetNextPowerOf2(unsigned(getSize().y)));
	m_storedSize = renderer->hasTextureNPOT() ? getSize() : nextPowerOfTwo;
	isNPOT = (getSize() != nextPowerOfTwo);
	
	return (tex != GL_NONE);
}

void GLTexture::upload() {
    arx_assert(tex != GL_NONE);

    glBindTexture(GL_TEXTURE_2D, tex);
    renderer->GetTextureStage(0)->current = this;
    
    if(!renderer->hasIntensityTextures() && isIntensity()) {
        arx_assert(getFormat() == Image::Format_L8);
        Image converted;
        converted.create(size_t(getStoredSize().x), size_t(getStoredSize().y), Image::Format_L8A8);
        unsigned char * input = m_image.getData();
        unsigned char * end = input + getStoredSize().x * getStoredSize().y;
        unsigned char * output = converted.getData();
        for(; input != end; input++) {
            *output++ = *input;
            *output++ = *input;
        }
        m_image = converted;
        m_format = Image::Format_L8A8;
        m_flags &= ~Intensity;
    }

    if(!renderer->hasBGRTextureTransfer()
       && (getFormat() == Image::Format_B8G8R8 || getFormat() == Image::Format_B8G8R8A8)) {
        Image::Format rgbFormat = getFormat() == Image::Format_B8G8R8 ? Image::Format_R8G8B8 : Image::Format_R8G8B8A8;
        m_image.convertTo(rgbFormat);
        m_format = rgbFormat;
    }

    if(getStoredSize() != getSize()) {
        m_flags &= ~HasMipmaps;
    }
    
    const bool isSupportedForEtc2 = (m_format == Image::Format_R8G8B8A8 ||
            m_format == Image::Format_B8G8R8A8 ||
            m_format == Image::Format_R8G8B8 ||
            m_format == Image::Format_B8G8R8 ||
            m_format == Image::Format_L8 ||
            m_format == Image::Format_A8 ||
            m_format == Image::Format_L8A8);
    
    if (!g_useGLES2_0 && g_EnableETC2TextureSupport && isSupportedForEtc2 && m_image.isValid()) {

        int upW = int(getStoredSize().x);
        int upH = int(getStoredSize().y);
        int finalW = int(((unsigned(upW) + 3) / 4) * 4);
        int finalH = int(((unsigned(upH) + 3) / 4) * 4);
        if (finalW < 4) finalW = 4;
        if (finalH < 4) finalH = 4;

        const unsigned char* srcData = nullptr;
        Image extended;

        if (finalW != int(getSize().x) || finalH != int(getSize().y)) {
            extended.create(size_t(finalW), size_t(finalH), m_image.getFormat());
            extended.extendClampToEdgeBorder(m_image);
            srcData = extended.getData();
        } else {
            srcData = m_image.getData();
        }

        if (UploadAsEtc2(tex, finalW, finalH, srcData, m_format, nullptr)) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            return;
        }
    }
    
    GLint internalUnsized, internalSized;
    GLenum format;
    if(isIntensity()) {
        internalUnsized = GL_INTENSITY, internalSized = GL_INTENSITY8, format = GL_RED;
    } else if(getFormat() == Image::Format_L8) {
        internalUnsized = GL_LUMINANCE, internalSized = GL_LUMINANCE8, format = GL_LUMINANCE;
    } else if(getFormat() == Image::Format_A8) {
        internalUnsized = GL_ALPHA, internalSized = GL_ALPHA8, format = GL_ALPHA;
    } else if(getFormat() == Image::Format_L8A8) {
        internalUnsized = GL_LUMINANCE_ALPHA, internalSized = GL_LUMINANCE8_ALPHA8, format = GL_LUMINANCE_ALPHA;
    } else if(getFormat() == Image::Format_R8G8B8) {
        internalUnsized = GL_RGB, internalSized = GL_RGB8, format = GL_RGB;
    } else if(getFormat() == Image::Format_B8G8R8) {
        internalUnsized = GL_RGB, internalSized = GL_RGB8, format = GL_BGR;
    } else if(getFormat() == Image::Format_R8G8B8A8) {
        internalUnsized = GL_RGBA, internalSized = GL_RGBA8, format = GL_RGBA;
    } else if(getFormat() == Image::Format_B8G8R8A8) {
        internalUnsized = GL_RGBA, internalSized = GL_RGBA8, format = GL_BGRA;
    } else {
        arx_assert_msg(false, "Unsupported image format: %ld", long(getFormat()));
        return;
    }
    GLint internal = renderer->hasSizedTextureFormats() ? internalSized : internalUnsized;
    
    if(hasMipmaps()) {
        glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
        if(renderer->getMaxAnisotropy() > 1.f) {
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, renderer->getMaxAnisotropy());
        }
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }

    if(getStoredSize() != getSize()) {
        Image extended;
        extended.create(size_t(getStoredSize().x), size_t(getStoredSize().y), m_image.getFormat());
        extended.extendClampToEdgeBorder(m_image);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, getStoredSize().x, getStoredSize().y, 0, format,
                     GL_UNSIGNED_BYTE, extended.getData());
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, internal, getSize().x, getSize().y, 0, format,
                     GL_UNSIGNED_BYTE, m_image.getData());
    }

}

void GLTexture::destroy() {
	
	if(tex) {
		glDeleteTextures(1, &tex), tex = GL_NONE;
	}
	
	for(size_t i = 0; i < renderer->getTextureStageCount(); i++) {
		GLTextureStage * stage = renderer->GetTextureStage(i);
		if(stage->tex == this) {
			stage->tex = nullptr;
		}
		if(stage->current == this) {
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

void GLTexture::apply(GLTextureStage * stage) {
	
	arx_assert(stage != nullptr);
	arx_assert(stage->tex == this);
	
	TextureStage::WrapMode newWrapMode = (!isNPOT) ? stage->getWrapMode()
	                                               : TextureStage::WrapClamp;
	if(newWrapMode != wrapMode) {
		wrapMode = newWrapMode;
		GLint glwrap = arxToGlWrapMode[wrapMode];
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, glwrap);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, glwrap);
	}
	
	if(stage->getMinFilter() != minFilter) {
		minFilter = stage->getMinFilter();
		int mipFilter = hasMipmaps() ? 1 : 0;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, arxToGlFilter[mipFilter][minFilter]);
	}
	
	if(stage->getMagFilter() != magFilter) {
		magFilter = stage->getMagFilter();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, arxToGlFilter[0][magFilter]);
	}
	
}

void GLTexture::updateMaxAnisotropy() {
	
	if(hasMipmaps()) {
		glBindTexture(GL_TEXTURE_2D, tex);
		renderer->GetTextureStage(0)->current = this;
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, renderer->getMaxAnisotropy());
	}
	
}
