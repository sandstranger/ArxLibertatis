#include "TextureCache.h"

#include <fstream>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>
#include <queue>
#include <thread>
#include <condition_variable>
#include <algorithm>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include "SDL_log.h"

const static bool g_enableTextureCache = true;
static const char CACHE_MAGIC[4] = {'T', 'C', '0', '1'};
static const uint32_t CACHE_VERSION = 1;

class BufferPool {
public:
    static BufferPool& Instance() {
        static BufferPool instance;
        return instance;
    }

    uint8_t* Acquire(size_t requestedSize) {
        if (requestedSize == 0) return nullptr;

        size_t bucketSize = RoundUpToPowerOfTwo(requestedSize);
        if (bucketSize < 4096) bucketSize = 4096;

        size_t idx = GetBucketIndex(bucketSize);
        if (idx >= NUM_BUCKETS) return nullptr;

        Bucket& bucket = m_buckets[idx];
        std::lock_guard<std::mutex> lock(bucket.mutex);

        if (bucket.freeBuffers.empty()) {
            uint8_t* newBuf = new (std::nothrow) uint8_t[bucketSize];
            if (!newBuf) return nullptr;
            bucket.allBuffers.push_back(newBuf);
            m_totalAllocated.fetch_add(bucketSize, std::memory_order_relaxed);
            return newBuf;
        }
        uint8_t* buf = bucket.freeBuffers.front();
        bucket.freeBuffers.pop();
        return buf;
    }

    void Release(uint8_t* buf, size_t originalRequestedSize) {
        if (!buf) return;

        size_t bucketSize = RoundUpToPowerOfTwo(originalRequestedSize);
        if (bucketSize < 4096) bucketSize = 4096;

        size_t idx = GetBucketIndex(bucketSize);
        if (idx >= NUM_BUCKETS) return;

        Bucket& bucket = m_buckets[idx];
        std::lock_guard<std::mutex> lock(bucket.mutex);
        bucket.freeBuffers.push(buf);
    }

    size_t GetTotalAllocated() const { return m_totalAllocated.load(std::memory_order_relaxed); }

    void Shutdown() {
        for (auto& bucket : m_buckets) {
            std::lock_guard<std::mutex> lock(bucket.mutex);
            for (uint8_t* buf : bucket.allBuffers) {
                delete[] buf;
            }
            bucket.allBuffers.clear();
            while (!bucket.freeBuffers.empty()) bucket.freeBuffers.pop();
        }
        m_totalAllocated.store(0, std::memory_order_relaxed);
    }

private:
    BufferPool() = default;
    ~BufferPool() { Shutdown(); }

    static inline size_t RoundUpToPowerOfTwo(size_t v) {
        if (v <= 1) return 1;
        return (size_t)1 << (64 - __builtin_clzll(v - 1));
    }

    static inline size_t GetBucketIndex(size_t size) {
        return 63 - __builtin_clzll(size) - 12;
    }

    struct Bucket {
        std::vector<uint8_t*> allBuffers;
        std::queue<uint8_t*> freeBuffers;
        std::mutex mutex;
    };

    static constexpr size_t NUM_BUCKETS = 20;
    Bucket m_buckets[NUM_BUCKETS];
    std::atomic<size_t> m_totalAllocated{0};
};


#pragma pack(push, 1)
struct CacheHeader {
    char magic[4];
    uint32_t version;
    uint64_t hash;
    uint32_t width;
    uint32_t height;
    uint32_t mipCount;
    uint32_t format;
    uint64_t timestamp;
    uint32_t dataSize;
};
#pragma pack(pop)

TextureCache& TextureCache::Instance() {
    static TextureCache instance;
    return instance;
}

void TextureCache::Init(const char* cacheDir, size_t maxSizeBytes) {
    std::lock_guard<std::mutex> lock(m_indexMutex);
    if (m_initialized) return;
    m_cacheDir = cacheDir;
    m_maxSizeBytes = maxSizeBytes;
    std::filesystem::path cachePath(m_cacheDir);
    std::error_code ec;
    std::filesystem::create_directories(cachePath, ec);
    m_currentSizeBytes = 0;
    
    if (std::filesystem::is_directory(cachePath, ec)) {
        for (const auto& dirEntry : std::filesystem::directory_iterator(cachePath)) {
            if (!dirEntry.is_regular_file()) continue;
            std::string name = dirEntry.path().filename().string();
            if (name.size() < 6 || name.substr(name.size() - 6) != ".cache") continue;

            std::string fullPath = dirEntry.path().string();
            struct stat fileStat;
            if (stat(fullPath.c_str(), &fileStat) != 0) continue;

            CacheEntry e;
            e.filename = fullPath;
            e.size = fileStat.st_size;
            e.lastAccessTime = fileStat.st_mtime;   
            e.hash = 0;
            m_entries[name] = e;
            m_currentSizeBytes += fileStat.st_size;
        }
    }

    m_initialized = true;
    m_shutdownRequested.store(false);
    m_workerThread = std::thread(&TextureCache::WorkerThreadFunc, this);

    struct sched_param param;
    param.sched_priority = 0;
    pthread_setschedparam(m_workerThread.native_handle(), SCHED_NORMAL, &param);

    SDL_Log("TextureCache initialized: %s %zu entries, %zu MB used\n",
           m_cacheDir.c_str(),m_entries.size(), m_currentSizeBytes / (1024 * 1024));
}

void TextureCache::Shutdown() {
    Flush();
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_shutdownRequested.store(true);
    }
    m_queueCV.notify_one();

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    std::lock_guard<std::mutex> lock(m_indexMutex);
    m_entries.clear();
    m_currentSizeBytes = 0;
    m_initialized = false;
}

void TextureCache::Flush() {
    while (m_pendingJobs.load(std::memory_order_relaxed) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::string TextureCache::GetCachePath(uint64_t hash) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%016llx.cache", (unsigned long long)hash);
    return m_cacheDir + "/" + buf;
}

bool TextureCache::ValidateCacheFile(const char* path, uint64_t expectedHash,
                                       int expectedWidth, int expectedHeight,
                                       int expectedFormat, int expectedMipCount) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    CacheHeader header;
    if (!file.read(reinterpret_cast<char*>(&header), sizeof(header))) {
        return false;
    }

    return (memcmp(header.magic, CACHE_MAGIC, 4) == 0 &&
            header.version == CACHE_VERSION &&
            header.hash == expectedHash &&
            (int)header.width == expectedWidth &&
            (int)header.height == expectedHeight &&
            (int)header.format == expectedFormat &&
            (int)header.mipCount == expectedMipCount);
}

bool TextureCache::TryGetCachedETC2(const char* textureName,
                                      const void* dxtData, size_t dxtSize,
                                      int width, int height, int format, int mipCount,
                                      std::vector<uint8_t>& outBuffer, size_t* outSize) {
    uint64_t hash = ComputeTextureHash(dxtData, dxtSize, width, height, format);
    std::string cachePath = GetCachePath(hash);
    std::string fileName = std::filesystem::path(cachePath).filename().string();

    {
        std::lock_guard<std::mutex> lock(m_indexMutex);
        if (!m_initialized) return false;

        auto it = m_entries.find(fileName);
        if (it == m_entries.end()) return false;

        it->second.lastAccessTime = time(nullptr);
    }

    
    if (!std::filesystem::exists(cachePath)) return false;

    if (!ValidateCacheFile(cachePath.c_str(), hash, width, height, format, mipCount)) {
        std::filesystem::remove(cachePath);
        std::lock_guard<std::mutex> lock(m_indexMutex);
        auto it = m_entries.find(fileName);
        if (it != m_entries.end()) {
            m_currentSizeBytes -= it->second.size;
            m_entries.erase(it);
        }
        return false;
    }

    std::ifstream file(cachePath, std::ios::binary);
    if (!file) return false;

    CacheHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file) return false;

    if (outBuffer.size() < header.dataSize) {
        outBuffer.resize(header.dataSize);
    }
    auto* buffer = reinterpret_cast<std::byte*>(outBuffer.data());
    if (!file.read(reinterpret_cast<char*>(buffer), header.dataSize)) {
        return false;
    }

    
    std::error_code ec;
    std::filesystem::last_write_time(cachePath, std::filesystem::file_time_type::clock::now(), ec);

    *outSize = header.dataSize;
    return true;
}

void TextureCache::SaveToCacheAsync(const char* textureName,
                                      const void* dxtData, size_t dxtSize,
                                      int width, int height, int format, int mipCount,
                                      const void* etc2Data, size_t etc2Size) {
    if (!m_initialized) return;

    if (m_pendingJobs.load(std::memory_order_relaxed) >= m_maxQueueSize.load(std::memory_order_relaxed)) {
        return;
    }

    uint64_t hash = ComputeTextureHash(dxtData, dxtSize, width, height, format);
    std::string cachePath = GetCachePath(hash);
    std::string fileName = std::filesystem::path(cachePath).filename().string();

    {
        std::lock_guard<std::mutex> lock(m_indexMutex);
        if (m_entries.find(fileName) != m_entries.end()) return;
    }

    uint8_t* buffer = BufferPool::Instance().Acquire(etc2Size);
    if (!buffer) return;
    memcpy(buffer, etc2Data, etc2Size);

    CacheWriteJob job;
    job.cachePath = cachePath;
    job.tempPath = cachePath + ".tmp";
    job.hash = hash;
    job.width = width;
    job.height = height;
    job.mipCount = mipCount;
    job.format = format;
    job.timestamp = time(nullptr);
    job.etc2Data = buffer;
    job.etc2Size = etc2Size;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_jobQueue.push(std::move(job));
        m_pendingJobs.fetch_add(1, std::memory_order_release);
    }
    m_queueCV.notify_one();
}

void TextureCache::WorkerThreadFunc() {
    while (true) {
        CacheWriteJob job;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCV.wait(lock, [this]() {
                return !m_jobQueue.empty() || m_shutdownRequested.load(std::memory_order_acquire);
            });

            if (m_shutdownRequested.load(std::memory_order_acquire) && m_jobQueue.empty()) {
                break;
            }

            job = std::move(m_jobQueue.front());
            m_jobQueue.pop();
        }

        ProcessJob(job);
        m_pendingJobs.fetch_sub(1, std::memory_order_release);
    }
}

void TextureCache::ProcessJob(const CacheWriteJob& job) {
    std::ofstream tempFile(job.tempPath, std::ios::binary);
    if (!tempFile) {
        BufferPool::Instance().Release(job.etc2Data, job.etc2Size);
        return;
    }

    CacheHeader header;
    memcpy(header.magic, CACHE_MAGIC, 4);
    header.version = CACHE_VERSION;
    header.hash = job.hash;
    header.width = job.width;
    header.height = job.height;
    header.mipCount = job.mipCount;
    header.format = job.format;
    header.timestamp = job.timestamp;
    header.dataSize = (uint32_t)job.etc2Size;

    tempFile.write(reinterpret_cast<const char*>(&header), sizeof(header));
    tempFile.write(reinterpret_cast<const char*>(job.etc2Data), job.etc2Size);
    tempFile.close();

    if (!tempFile) {
        std::error_code ec;
        std::filesystem::remove(job.tempPath, ec);
        BufferPool::Instance().Release(job.etc2Data, job.etc2Size);
        return;
    }

    std::error_code ec;
    std::filesystem::rename(job.tempPath, job.cachePath, ec);
    if (ec) {
        std::filesystem::remove(job.tempPath, ec);
        BufferPool::Instance().Release(job.etc2Data, job.etc2Size);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_indexMutex);
        std::string fileName = std::filesystem::path(job.cachePath).filename().string();
        CacheEntry entry;
        entry.filename = job.cachePath;
        entry.hash = job.hash;
        entry.size = sizeof(CacheHeader) + job.etc2Size;
        entry.lastAccessTime = job.timestamp;

        m_entries[fileName] = entry;
        m_currentSizeBytes += entry.size;

        EvictIfNeeded();
    }

    BufferPool::Instance().Release(job.etc2Data, job.etc2Size);
}

void TextureCache::EvictIfNeeded() {
    if (m_currentSizeBytes <= m_maxSizeBytes) return;

    std::vector<std::pair<std::string, CacheEntry*>> sorted;
    sorted.reserve(m_entries.size());
    for (auto& kv : m_entries) {
        sorted.push_back({kv.first, &kv.second});
    }

    size_t toRemove = sorted.size() / 5;
    if (toRemove < 1) toRemove = 1;

    std::nth_element(sorted.begin(), sorted.begin() + toRemove, sorted.end(),
                     [](const auto& a, const auto& b) {
                         return a.second->lastAccessTime < b.second->lastAccessTime;
                     });

    for (size_t i = 0; i < toRemove && i < sorted.size(); ++i) {
        const std::string& name = sorted[i].first;
        CacheEntry* entry = sorted[i].second;

        std::error_code ec;
        std::filesystem::remove(entry->filename, ec);
        if (!ec) {
            m_currentSizeBytes -= entry->size;
            m_entries.erase(name);
        }
    }
}

bool TextureCache::TryGetFromRamCache(uint64_t hash, std::vector<uint8_t>& outBuffer, size_t* outSize) {
    size_t dataSize = 0;
    {
        std::lock_guard<std::mutex> lock(m_ramCacheMutex);
        auto it = m_ramCacheIndex.find(hash);
        if (it == m_ramCacheIndex.end()) return false;
        dataSize = it->second->etc2Size;
    }

    if (outBuffer.size() < dataSize) {
        outBuffer.resize(dataSize);
    }
    uint8_t* buffer = outBuffer.data();
    if (!buffer) return false;

    {
        std::lock_guard<std::mutex> lock(m_ramCacheMutex);
        auto it = m_ramCacheIndex.find(hash);
        if (it == m_ramCacheIndex.end()) return false;

        auto& entry = *it->second;
        m_ramCacheList.splice(m_ramCacheList.begin(), m_ramCacheList, it->second);

        static thread_local uint64_t accessCounter = 0;
        if (++accessCounter % 128 == 0) {
            entry.lastAccessTime = std::chrono::steady_clock::now().time_since_epoch().count();
        }

        memcpy(buffer, entry.etc2Data, dataSize);
    }

    *outSize = dataSize;
    return true;
}

void TextureCache::SaveToRamCache(uint64_t hash, const void* etc2Data, size_t etc2Size,
                                    uint32_t width, uint32_t height, uint32_t format) {
    if (etc2Data == nullptr || etc2Size == 0) return;

    uint8_t* buffer = BufferPool::Instance().Acquire(etc2Size);
    if (!buffer) return;
    memcpy(buffer, etc2Data, etc2Size);

    std::lock_guard<std::mutex> lock(m_ramCacheMutex);

    if (m_ramCacheIndex.find(hash) != m_ramCacheIndex.end()) {
        BufferPool::Instance().Release(buffer, etc2Size);
        return;
    }

    m_ramCacheList.emplace_front();
    auto& entry = m_ramCacheList.front();

    entry.hash = hash;
    entry.etc2Data = buffer;
    entry.etc2Size = etc2Size;
    entry.width = width;
    entry.height = height;
    entry.format = format;
    entry.lastAccessTime = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    entry.size = etc2Size;

    m_ramCacheIndex.emplace(hash, m_ramCacheList.begin());
    m_ramCacheCurrentSize += etc2Size;

    EvictRamCacheIfNeeded();
}

void TextureCache::EvictRamCacheIfNeeded() {
    if (m_ramCacheCurrentSize <= m_ramCacheMaxSize) return;

    size_t toRemove = m_ramCacheList.size() / 5;
    if (toRemove < 1) toRemove = 1;

    for (size_t i = 0; i < toRemove && !m_ramCacheList.empty(); ++i) {
        auto& oldest = m_ramCacheList.back();
        m_ramCacheCurrentSize -= oldest.size;
        m_ramCacheIndex.erase(oldest.hash);
        BufferPool::Instance().Release(oldest.etc2Data, oldest.etc2Size);
        m_ramCacheList.pop_back();
    }
}

void TextureCache::ClearRamCache() {
    std::lock_guard<std::mutex> lock(m_ramCacheMutex);
    for (auto& entry : m_ramCacheList) {
        BufferPool::Instance().Release(entry.etc2Data, entry.etc2Size);
    }
    m_ramCacheList.clear();
    m_ramCacheIndex.clear();
    m_ramCacheCurrentSize = 0;
}

void TextureCache::ForceEvictRamCache(float fraction) {
    std::lock_guard<std::mutex> lock(m_ramCacheMutex);

    size_t toRemove = static_cast<size_t>(m_ramCacheList.size() * fraction);
    if (toRemove < 1 && !m_ramCacheList.empty()) toRemove = 1;

    for (size_t i = 0; i < toRemove && !m_ramCacheList.empty(); ++i) {
        auto& oldest = m_ramCacheList.back();
        m_ramCacheCurrentSize -= oldest.size;
        m_ramCacheIndex.erase(oldest.hash);
        BufferPool::Instance().Release(oldest.etc2Data, oldest.etc2Size);
        m_ramCacheList.pop_back();
    }
}

void ClearRamCache() {
    TextureCache::Instance().Flush();
    TextureCache::Instance().ClearRamCache();
}

