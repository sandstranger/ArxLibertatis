#pragma once
#include <unordered_map>
#include <string>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <list>
#include <iostream>
#include <vector>
#include <cstdint>

#include "platform/Platform.h"

inline uint64_t FNV1a_Hash(const void* data, size_t size) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline uint64_t ComputeTextureHash(const void* dxtData, size_t dxtSize,
                                   int width, int height, int format) {
    uint64_t h = FNV1a_Hash(dxtData, dxtSize);
    h ^= FNV1a_Hash(&width, sizeof(width));
    h ^= FNV1a_Hash(&height, sizeof(height));
    h ^= FNV1a_Hash(&format, sizeof(format));
    return h;
}

struct RamCacheEntry {
    uint64_t hash;
    uint8_t* etc2Data;
    size_t etc2Size;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t lastAccessTime;
    size_t size;                // the memory the entry holds, see BufferPool::AllocationSize
};

struct CacheEntry {
    uint64_t hash;
    std::string filename;
    uint64_t lastAccessTime;
    size_t size;
};

struct CacheWriteJob {
    std::string cachePath;
    std::string tempPath;
    uint8_t* etc2Data;
    size_t etc2Size;
    uint64_t hash;
    uint32_t width;
    uint32_t height;
    uint32_t mipCount;
    uint32_t format;
    uint64_t timestamp;
};

class TextureCache {
public:
    static TextureCache& Instance();

    // 64 bit sizes: in a 32 bit process a size_t limit of 30 GB wrapped to 2 GB.
    void Init(const char* cacheDir, uint64_t maxSizeBytes = 512ULL * 1024ULL * 1024ULL);
    void Shutdown();

    bool TryGetCachedETC2(const char* textureName,
                          const void* dxtData, size_t dxtSize,
                          int width, int height, int format, int mipCount,
                          std::vector<uint8_t>& outBuffer, size_t* outSize);

    // The same with the hash of ComputeTextureHash computed once by the caller: it reads every byte
    // of the source image, which is slow with 64 bit multiplies on ARMv7.
    bool TryGetCachedETC2(uint64_t hash, int width, int height, int format, int mipCount,
                          std::vector<uint8_t>& outBuffer, size_t* outSize);

    void SaveToCacheAsync(const char* textureName,
                          const void* dxtData, size_t dxtSize,
                          int width, int height, int format, int mipCount,
                          const void* etc2Data, size_t etc2Size);

    void SaveToCacheAsync(uint64_t hash, int width, int height, int format, int mipCount,
                          const void* etc2Data, size_t etc2Size);

    size_t GetPendingJobsCount() const { return m_pendingJobs.load(); }
    void Flush();

    bool TryGetFromRamCache(uint64_t hash,
                            std::vector<uint8_t>& outBuffer, size_t* outSize);

    void SaveToRamCache(uint64_t hash, const void* etc2Data, size_t etc2Size,
                        uint32_t width, uint32_t height, uint32_t format);
    void EvictRamCacheIfNeeded();
    size_t GetRamCacheSize() const { return m_ramCacheCurrentSize; }
    void ClearRamCache();
    // ClearRamCache without waiting for the lock, for the out of memory handler (the failed
    // allocation may come from inside the cache) and for the low memory event of another thread.
    // Returns the bytes given back to the system.
    size_t TryClearRamCache();
    void ForceEvictRamCache(float fraction);

private:
    TextureCache() = default;
    ~TextureCache() = default;

    std::string GetCachePath(uint64_t hash);
    bool ValidateCacheFile(const char* path, uint64_t expectedHash,
                           int expectedWidth, int expectedHeight,
                           int expectedFormat, int expectedMipCount);

    void WorkerThreadFunc();
    void ProcessJob(const CacheWriteJob& job);
    void EvictIfNeeded();
    void ReleaseRamCacheEntriesLocked();

    std::string m_cacheDir;
    uint64_t m_maxSizeBytes = 0;
    uint64_t m_currentSizeBytes = 0;
    std::unordered_map<std::string, CacheEntry> m_entries;
    std::mutex m_indexMutex;
    bool m_initialized = false;

    std::mutex m_queueMutex;
    std::condition_variable m_queueCV;
    std::queue<CacheWriteJob> m_jobQueue;
    std::thread m_workerThread;
    std::atomic<bool> m_shutdownRequested{false};
    std::atomic<size_t> m_pendingJobs{0};
    std::atomic<size_t> m_maxQueueSize{64};

    // Every queued job holds a copy of its texture until it is on disk. Counting jobs only let a
    // level load queue 64 copies, hundreds of MB in power of two buffers: past this many bytes
    // SaveToCacheAsync waits for the writer instead. Guarded by m_queueMutex.
    size_t m_pendingBytes = 0;
    std::condition_variable m_jobDoneCV;
#if ARX_LOW_MEMORY
    static constexpr size_t m_maxPendingBytes = 16 * 1024 * 1024;
#else
    static constexpr size_t m_maxPendingBytes = 64 * 1024 * 1024;
#endif

    std::unordered_map<uint64_t, std::list<RamCacheEntry>::iterator> m_ramCacheIndex;
    std::list<RamCacheEntry> m_ramCacheList;
    std::mutex m_ramCacheMutex;
    // It only spares reading the disk cache again, the textures are in GL memory already: on ARMv7
    // 200 MB of it was a good part of what the process can have at all.
#if ARX_LOW_MEMORY
    size_t m_ramCacheMaxSize = 16 * 1024 * 1024;
#else
    size_t m_ramCacheMaxSize = 200 * 1024 * 1024;
#endif
    size_t m_ramCacheCurrentSize = 0;
};
