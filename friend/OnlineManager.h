#pragma once

#include <unordered_map>
#include <mutex>
#include <chrono>
#include <functional>
#include <vector>
#include "logging.h"
#include "mysql/userDAO.h"

class OnlineManager {
public:
    OnlineManager() = default;
    static OnlineManager& getInstance() {
        static OnlineManager instance;
        return instance;
    }

    // 更新心跳时间
    void updateHeartbeat(uint64_t user_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        bool was_offline = (heartbeat_map.find(user_id) == heartbeat_map.end());
        heartbeat_map[user_id] = std::chrono::steady_clock::now();
        
        // 只在用户首次上线时更新数据库状态
        if (was_offline) {
            UserDAO dao;
            dao.updateUserStatus(user_id, 1);
            LOG_DEBUG << "User " << user_id << " came online";
        }
    }

    // 获取用户的最后在线时间
    std::chrono::steady_clock::time_point getLastActive(uint64_t user_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = heartbeat_map.find(user_id);
        if (it != heartbeat_map.end()) {
            return it->second;
        }
        // 返回一个非常旧的时间点，确保被视为离线
        return std::chrono::steady_clock::time_point::min();
    }

    // 检查用户是否在线
    bool isOnline(uint64_t user_id, int timeout_seconds = 30) {
        auto last = getLastActive(user_id);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last).count();
        return elapsed < timeout_seconds;
    }

    // 超时检测清理离线用户
    void checkTimeout(int timeout_seconds = 30) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        std::vector<uint64_t> to_offline;
        
        for (auto it = heartbeat_map.begin(); it != heartbeat_map.end(); ++it) {
            uint64_t uid = it->first;
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
            if (elapsed >= timeout_seconds) {
                to_offline.push_back(uid);
            }
        }

        if (to_offline.empty()) {
            return;
        }

        UserDAO dao;
        for (uint64_t uid : to_offline) {
            heartbeat_map.erase(uid);
            dao.updateUserStatus(uid, 0);
            LOG_DEBUG << "User " << uid << " marked offline (timeout)";
        }
    }

    // 用户退出时主动离线
    void removeUser(uint64_t user_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (heartbeat_map.erase(user_id) > 0) {
            UserDAO dao;
            dao.updateUserStatus(user_id, 0);
            LOG_DEBUG << "User " << user_id << " removed from online manager (logout)";
        }
    }

    // 获取所有在线的用户ID
    std::vector<uint64_t> getOnlineUsers(int timeout_seconds = 30) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<uint64_t> online;
        online.reserve(heartbeat_map.size());
        auto now = std::chrono::steady_clock::now();
        
        for (const auto& pair : heartbeat_map) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - pair.second).count();
            if (elapsed < timeout_seconds) {
                online.push_back(pair.first);
            }
        }
        return online;
    }

private:
    std::mutex mutex_;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> heartbeat_map;
};