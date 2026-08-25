#pragma once

#include <unordered_map>
#include <mutex>
#include <chrono>
#include <functional>
#include <vector>
#include "tool/logging.h"
#include "mysql/userDAO.h"

class OnlineManager {
public:
    OnlineManager() = default;
    static OnlineManager& getInstance() {
        static OnlineManager instance;
        return instance;
    }

    // void setTimeoutCallback(std::function<void(uint64_t)> callback) {
    //     timeout_callback_ = callback;
    // }

    void setBusyCheckCallback(std::function<bool(uint64_t)> callback) {
        busy_check_callback_ = callback;
    }

    // 用户主动上线
    void userOnline(uint64_t user_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        bool was_offline = (heartbeat_map.find(user_id) == heartbeat_map.end());
        heartbeat_map[user_id] = std::chrono::steady_clock::now();
        
        if (was_offline) {
            std::thread([user_id]() {
                UserDAO dao;
                dao.updateUserStatus(user_id, 1);
            }).detach();
            LOG_DEBUG << "User " << user_id << " login";
        }
    }
    static constexpr int TIMEOUT = 90;

    // 更新心跳时间
    // void updateHeartbeat(uint64_t user_id) {
    //     std::lock_guard<std::mutex> lock(mutex_);
    //     bool was_offline = (heartbeat_map.find(user_id) == heartbeat_map.end());
    //     heartbeat_map[user_id] = std::chrono::steady_clock::now();
        
    //     if (was_offline) {
    //         UserDAO dao;
    //         dao.updateUserStatus(user_id, 1);
    //         LOG_DEBUG << "User " << user_id << " came online (heartbeat)";
    //     }
    // }

    // 检查用户是否在线
    bool isOnline(uint64_t user_id, int timeout_seconds = TIMEOUT) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = heartbeat_map.find(user_id);
        if (it == heartbeat_map.end()) {
            return false;
        }
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
        
        // 只返回状态，不自动删除
        return elapsed < timeout_seconds;
}
    // 超时检测清理离线用户
    void checkTimeout(int timeout_seconds = TIMEOUT) {
        auto now = std::chrono::steady_clock::now();
        std::vector<uint64_t> to_offline;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& pair : heartbeat_map) {
                uint64_t uid = pair.first;
                if (busy_check_callback_ && busy_check_callback_(uid)) {
                    continue;
                }
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - pair.second).count();
                if (elapsed >= timeout_seconds) {
                    to_offline.push_back(uid);
                }
            }

            if (to_offline.empty()) {
                return;
            }

            for (uint64_t uid : to_offline) {
                heartbeat_map.erase(uid);
            }
        }

        // 锁外执行 DB 写和回调，避免回调removeUser再次加锁导致死锁
        for (uint64_t uid : to_offline) {
            UserDAO dao;
            dao.updateUserStatus(uid, 0);

            if (timeout_callback_) {
                timeout_callback_(uid);
            }
            LOG_DEBUG << "User " << uid << " marked offline (timeout)";
        }
    }

    // 用户主动离线
    void userOffline(uint64_t user_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (heartbeat_map.erase(user_id) > 0) {
            UserDAO dao;
            dao.updateUserStatus(user_id, 0);
            LOG_DEBUG << "User " << user_id << " went offline (logout)";
        }
        else {
            UserDAO dao;
            dao.updateUserStatus(user_id, 0);
            LOG_INFO << "User " << user_id << "force dffline";
        }
    }

    // 用户退出时主动离线
    void removeUser(uint64_t user_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (heartbeat_map.erase(user_id) > 0) {
            UserDAO dao;
            dao.updateUserStatus(user_id, 0);
            LOG_DEBUG << "User " << user_id << " removed from online manager (disconnect)";
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

    // 清除所有在线状态
    void clearAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (heartbeat_map.empty()) {
            return;
        }
        
        UserDAO dao;
        for (const auto& pair : heartbeat_map) {
            dao.updateUserStatus(pair.first, 0);
        }
        heartbeat_map.clear();
        LOG_INFO << "All users cleared from online manager";
    }

private:
    std::mutex mutex_;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> heartbeat_map;
    std::function<void(uint64_t)> timeout_callback_;
    std::function<bool(uint64_t)> busy_check_callback_;
};