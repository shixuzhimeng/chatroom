// 频率限制器
#pragma once

#include <chrono>
#include <unordered_map>
#include <mutex>
#include <deque>
#include <string> 

class Limiter {
public:
    Limiter(size_t max_requests, int64_t time)
        : max_requests_(max_requests), time_(time) {};
    
    bool isallow(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        auto& queue = records_[key];
    
        // 清理过期记录
        auto clean = now - std::chrono::milliseconds(time_);
        while(!queue.empty() && queue.front() < clean) {
            queue.pop_front();
        }
        // 超出限制
        if(queue.size() >= max_requests_) {
            return false;
        }
        
        queue.push_back(now);
        return true;
    }

    // 剩余的请求次数
    size_t remaining(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        auto& queue = records_[key];
        auto clean = now- std::chrono::milliseconds(time_);

        while(!queue.empty() && queue.front() < clean) {
            queue.pop_front();
        }

        if(queue.size() >= max_requests_) {
            return 0;
        }

        return max_requests_ - queue.size();
    }

    // 重置次数
    void reset(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.erase(key);
    }


private:
    size_t max_requests_;
    int64_t time_;
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> records_;
    std::mutex mutex_;

};


class LimiterManage {
public:
    static LimiterManage& getInstance() {
        static  LimiterManage instance;
        return instance;
    }

    LimiterManage() = default;

    // 不同业务的限制器
    Limiter& getMessageLimit() {
        static Limiter limit(30, 1000);
        return limit;
    }

    Limiter& getRequestLimit() {
        static Limiter limit(30, 1000);
        return limit;
    }

    Limiter& getLoginLimit() {
        static Limiter limit(5, 60000);
        return limit;
    }
};