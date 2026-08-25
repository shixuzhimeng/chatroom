#pragma once

// 用户会话管理

#include "HashSalt.h"
#include "tool/logging.h"
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>


class TManager {
public:
    static TManager& getInstance() {
        static TManager instance;
        return instance;
    }


    struct TInfo {
        uint64_t user_id;
        std::string username;
        std::chrono::steady_clock::time_point expire_time;
        std::string device_id;
    };

    // 生成临时身份ID
    std::string generateT(uint64_t user_id, const std::string& username, const std::string& device_id = "", int expire_hours = 24) {
        std::lock_guard<std::mutex> lock(mutex_);
    
        std::string tID = Crypot::generateID(32);

        TInfo info;
        info.user_id = user_id;
        info.username = username;
        info.device_id = device_id;
        info.expire_time = std::chrono::steady_clock::now() + std::chrono::hours(expire_hours); // 有效期

        Tcache_[tID] = info;
        user_t_[user_id].push_back(tID);

        LOG_INFO << "generate ID for user: " << username << "(uid=" << user_id << ")";

        return tID;
    }

    // 验证临时身份ID
    bool verifyT(const std::string& tID, TInfo& info) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = Tcache_.find(tID);
        if(it == Tcache_.end()) {
            LOG_ERROR << "Not found tID: " << tID.substr(0, 8) << "...";
            return false;
        }

        auto now = std::chrono::steady_clock::now();
        if(now > it->second.expire_time) {
            Tcache_.erase(it);
            LOG_ERROR << "the ID expired";
            return false;
        }

        info = it->second;
        return true;
    }

    // 刷新身份ID所有效期
    bool refresh(const std::string& tID, int extra_hours = 24) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = Tcache_.find(tID);
        if(it == Tcache_.end()) {
            return false;
        }
        
        it->second.expire_time = std::chrono::steady_clock::now() + std::chrono::hours(extra_hours);

        LOG_INFO << "tID refreshed";
        return true;
    }

    // 清除临时身份ID
    bool revoketID(const std::string& tID) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = Tcache_.find(tID);
        if(it == Tcache_.end()) {
            return false;
        }

        uint64_t user_id = it->second.user_id;
        Tcache_.erase(it);

        auto ut_it = user_t_.find(user_id);
        if(ut_it != user_t_.end()) {
            auto& tlist = ut_it->second;
            tlist.erase(std::remove(tlist.begin(), tlist.end(), tID), tlist.end());
            if(tlist.empty()) {
                user_t_.erase(ut_it);
            }
        }

        LOG_INFO << "tID revoke for user_id: " << user_id;
        return true;
    }

    // 清理一个用户的所有的临时身份ID
    void revokeAlltID(uint64_t user_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = user_t_.find(user_id);
        if(it != user_t_.end()) {
            for(const auto& tID : it->second) {
                Tcache_.erase(tID);
            }
            user_t_.erase(it);
            LOG_INFO << "All tID revoked for user_id" << user_id;
        }
    }

    // 获取当前用户的所有的有效的临时身份ID
    std::vector<std::string> getUserT(uint64_t user_id) {
        std::lock_guard<std::mutex> lock(mutex_);
    
        auto it = user_t_.find(user_id);
        if(it != user_t_.end()) {
            return it->second;
        }

        return {};
    }

    //清理过期的身份ID
    void cleanExprie() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        std::vector<std::string> to_remove;
        for(const auto& pair : Tcache_) {
            const auto& tID = pair.first;
            const auto& info = pair.second;
            if(now > info.expire_time) {
                to_remove.push_back(tID);
            }
        }

        for(const auto& tID : to_remove) {
            Tcache_.erase(tID);
            for(auto& pair : user_t_) {
                auto& tlist = pair.second;
                tlist.erase(std::remove(tlist.begin(), tlist.end(), tID), tlist.end());
            }
        }

        for(auto it = user_t_.begin(); it != user_t_.end(); ) {
            if(it->second.empty()) {
                it = user_t_.erase(it);
            }
            else {
                ++it;
            }

        }
    }

private:
    TManager() = default;

    std::mutex mutex_;
    std::unordered_map<std::string, TInfo> Tcache_;
    std::unordered_map<uint64_t, std::vector<std::string>> user_t_;    
};