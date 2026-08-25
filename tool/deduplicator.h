#pragma once

#include <unordered_map>
#include <mutex>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>

class MessageDeduplicator {
public:
    static MessageDeduplicator& getInstance() {
        static MessageDeduplicator instance;
        return instance;
    }

    // 检查消息是否已经处理过了
    bool isDuplicate(uint64_t msg_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanExpiredLocked();  // 清理过期记录
        return records_.find(msg_id) != records_.end();
    }

    // 标记消息为已处理
    void markProcessed(uint64_t msg_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        records_[msg_id] = std::chrono::steady_clock::now();
    }

    // 批量标记
    void markProcessedBatch(const std::vector<uint64_t>& msg_ids) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto id : msg_ids) {
            records_[id] = now;
        }
    }

    // 清除所有记录
    void clearAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.clear();
    }

    // 清理过期记录（外部可调用）
    void cleanExpired() {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanExpiredLocked();
    }

private:
    MessageDeduplicator() = default;

    // 清理过期记录
    void cleanExpiredLocked() {
        const int64_t DEDUP_WINDOW_MS = 5000;  // 5秒去重窗口
        auto now = std::chrono::steady_clock::now();
        auto cutoff = now - std::chrono::milliseconds(DEDUP_WINDOW_MS);

        for (auto it = records_.begin(); it != records_.end(); ) {
            if (it->second < cutoff) {
                it = records_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::mutex mutex_;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> records_;
};