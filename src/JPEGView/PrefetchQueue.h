#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <unordered_map>
#include <memory>

class CJPEGImage;

// Intelligent bidirectional prefetch cache for zero-latency image navigation
class CPrefetchQueue {
public:
    static CPrefetchQueue& This() {
        static CPrefetchQueue instance;
        return instance;
    }

    void Start(int workerCount = 2) {
        if (m_running.exchange(true)) return;

        for (int i = 0; i < workerCount; ++i) {
            m_workers.emplace_back(&CPrefetchQueue::WorkerThread, this);
        }
    }

    void Stop() {
        if (!m_running.exchange(false)) return;

        m_cv.notify_all();
        for (auto& worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        m_workers.clear();
        ClearCache();
    }

    // Set current active image and enqueue adjacent files for prefetching
    void RequestPrefetch(const std::vector<std::wstring>& fileList, int currentIndex, int prefetchForward = 2, int prefetchBackward = 1) {
        if (fileList.empty() || !m_running) return;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();

        // Forward prefetch priorities (N+1, N+2...)
        for (int i = 1; i <= prefetchForward; ++i) {
            int targetIdx = currentIndex + i;
            if (targetIdx < static_cast<int>(fileList.size())) {
                const auto& path = fileList[targetIdx];
                if (m_cache.find(path) == m_cache.end()) {
                    m_queue.push_back(path);
                }
            }
        }

        // Backward prefetch priority (N-1)
        for (int i = 1; i <= prefetchBackward; ++i) {
            int targetIdx = currentIndex - i;
            if (targetIdx >= 0) {
                const auto& path = fileList[targetIdx];
                if (m_cache.find(path) == m_cache.end()) {
                    m_queue.push_back(path);
                }
            }
        }

        m_cv.notify_all();
    }

    // Retrieve prefetched image if ready
    std::shared_ptr<CJPEGImage> GetCached(const std::wstring& path) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_cache.find(path);
        if (it != m_cache.end()) {
            auto img = it->second;
            return img;
        }
        return nullptr;
    }

    void ClearCache() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache.clear();
        m_queue.clear();
    }

private:
    CPrefetchQueue() = default;
    ~CPrefetchQueue() {
        Stop();
    }

    void WorkerThread() {
        while (m_running) {
            std::wstring targetPath;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this]() {
                    return !m_running || !m_queue.empty();
                });

                if (!m_running) break;

                if (!m_queue.empty()) {
                    targetPath = m_queue.front();
                    m_queue.pop_front();
                }
            }

            if (!targetPath.empty()) {
                // Background decode execution
                // Cache management with max size constraint (e.g. 5 items)
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_cache.size() >= 6) {
                    m_cache.erase(m_cache.begin());
                }
            }
        }
    }

    std::atomic<bool> m_running{ false };
    std::vector<std::thread> m_workers;
    std::deque<std::wstring> m_queue;
    std::unordered_map<std::wstring, std::shared_ptr<CJPEGImage>> m_cache;
    std::mutex m_mutex;
    std::condition_variable m_cv;
};
