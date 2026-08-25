#pragma once

#include "HashSalt.h"
#include "tool/logging.h"
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <string>


class YanZheng {
public:
    YanZheng() = default;
    static YanZheng& getInstance() {
        static YanZheng instance;
        return instance;
    }

    // 生成验证码并返回
    std::string generateCode(const std::string& key, int expire_seconds = 300) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::string code = Crypot::generateYanZengCode();
        CodeInfo info{code, std::chrono::steady_clock::now() + std::chrono::seconds(expire_seconds)};
        cache_[key] = info;

        LOG_DEBUG << "YanZheng code generated for key: " << key << " , code: " << code;
        return code;
    }

    // 验证验证码
    bool verifycode(const std::string& key, const std::string& code) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = cache_.find(key);
        if(it == cache_.end()) {
            LOG_ERROR << "Verify code not found for key: " << key;
            return false;
        }

        auto now = std::chrono::steady_clock::now();
        if(now > it->second.expire_time) {
            cache_.erase(it);
            LOG_ERROR << "Verify code expired for key: " << key;
            return false;
        }


        if(it->second.code != code) {
            LOG_ERROR << "Verify code mismatch for key: " << key; 
            return false;
        }

        cache_.erase(it);
        LOG_DEBUG << "the code verify for key: " << key;
        return true;

    }

    // 清理所有的验证码
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
    }

    // 清除过期的验证码
    void cleanExpired() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        for(auto it = cache_.begin(); it != cache_.end();) {
            if(now > it->second.expire_time) {
                it = cache_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    

private:
    std::mutex mutex_;
    struct CodeInfo {
        std::string code;
        std::chrono::steady_clock::time_point expire_time;
    };
    std::unordered_map<std::string, CodeInfo> cache_;
};