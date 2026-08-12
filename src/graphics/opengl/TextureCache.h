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
    size_t size;                
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

    void Init(const char* cacheDir, size_t maxSizeBytes = 512ULL * 1024ULL * 1024ULL);
    void Shutdown();

    bool TryGetCachedETC2(const char* textureName,
                          const void* dxtData, size_t dxtSize,
                          int width, int height, int format, int mipCount,
                          std::vector<uint8_t>& outBuffer, size_t* outSize);

    void SaveToCacheAsync(const char* textureName,
                          const void* dxtData, size_t dxtSize,
                          int width, int height, int format, int mipCount,
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

    std::string m_cacheDir;
    size_t m_maxSizeBytes = 0;
    size_t m_currentSizeBytes = 0;
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

    std::unordered_map<uint64_t, std::list<RamCacheEntry>::iterator> m_ramCacheIndex;
    std::list<RamCacheEntry> m_ramCacheList;
    std::mutex m_ramCacheMutex;
    size_t m_ramCacheMaxSize = 200 * 1024 * 1024;
    size_t m_ramCacheCurrentSize = 0;
};
