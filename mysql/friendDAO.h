#pragma once

#include "baseDAO.h"
#include "userDAO.h"
#include <vector>
#include <map>
#include "../TranscationGuard.h"

// 好友请求
struct FriendRequest {
    uint64_t request_id;
    uint64_t from_uid;
    uint64_t to_uid;
    std::string message;
    int status;  // 0待处理、1已接受、2已拒绝、3已忽略
    int64_t created_at;
    int64_t updated_at;
};

// 好友关系
struct FriendShip {
    uint64_t friendship_id;
    uint64_t user_id;
    uint64_t friend_id;
    int status;  // 0待定、1已接受、2已拉黑、3已删除
    std::string remark;
    std::string group_name;
    int64_t created_at;
    int64_t updated_at;
};

struct FriendRequestInfo {
    uint64_t request_id;
    uint64_t from_uid;
    uint64_t to_uid;
    std::string message;
    int status;
    int64_t created_at;
};

class FriendDAO : public BaseDAO {
public:
    // 建立好友关系
    bool addFriendship(uint64_t user_id, uint64_t friend_id, const std::string& remark = "") {
        TransactionGuard tx(*this);
        if(!addFriendshipNoTx(user_id, friend_id, remark)) {
            return false;
        }
        tx.commit();
        return true;
    }

    // 删除好友
    bool deleteFriend(uint64_t user_id, uint64_t friend_id) {
        std::string sql = "DELETE FROM friendships WHERE (user_id = " + std::to_string(user_id) + 
                            " AND friend_id = " + std::to_string(friend_id) + 
                            ") OR (user_id = " + std::to_string(friend_id) + 
                            " AND friend_id = " + std::to_string(user_id) + ")";

        return executeUpdate(sql);
    }

    // 查询好友列表
    std::vector<USER> getFriend(uint64_t user_id, bool include_offline = true) {
        std::vector<USER> friends;
        
        // 使用 UNION 查询双向关系
        std::string sql = "SELECT u.user_id, u.username, u.nickname, u.avatar, u.status "
                        "FROM users u "
                        "WHERE u.user_id IN ("
                        "  SELECT f.friend_id "
                        "  FROM friendships f "
                        "  WHERE f.user_id = " + std::to_string(user_id) + " "
                        "    AND f.status = 1 "
                        "  UNION "
                        "  SELECT f.user_id "
                        "  FROM friendships f "
                        "  WHERE f.friend_id = " + std::to_string(user_id) + " "
                        "    AND f.status = 1 "
                        ") "
                        "AND u.user_id != " + std::to_string(user_id);
        
        if (!include_offline) {
            sql += " AND u.status IN (1, 2, 3)";
        }
        
        LOG_DEBUG << "getFriend SQL: " << sql;
        
        std::vector<std::map<std::string, std::string>> result;
        if (!executeQuery(sql, result)) {
            LOG_ERROR << "getFriend: executeQuery failed for user " << user_id;
            return friends;
        }
        
        LOG_DEBUG << "getFriend: query returned " << result.size() << " rows";
        
        for (const auto& row : result) {
            try {
                USER user;
                user.user_id = std::stoull(row.at("user_id"));
                user.username = row.at("username");
                user.nickname = row.at("nickname");
                user.avatar = row.at("avatar");
                user.status = std::stoi(row.at("status"));
                friends.push_back(user);
                LOG_DEBUG << "getFriend: found friend user_id=" << user.user_id 
                        << ", nickname=" << user.nickname;
            } catch (const std::exception& e) {
                LOG_ERROR << "getFriend: error parsing row: " << e.what();
            }
        }
        
        LOG_INFO << "getFriend: found " << friends.size() << " friends for user " << user_id;
        return friends;
    }

    // 获取好友关系
    bool getFriendShip(uint64_t user_id, uint64_t friend_id, FriendShip& friendship) {
        std::string sql = "SELECT * FROM friendships WHERE status = 1 AND ("
                        "(user_id = " + std::to_string(user_id) + 
                        " AND friend_id = " + std::to_string(friend_id) + 
                        ") OR (user_id = " + std::to_string(friend_id) + 
                        " AND friend_id = " + std::to_string(user_id) + 
                        "))";
        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result) || result.empty()) {
            return false;
        }
        fillFriendshipFromMap(result[0], friendship);
        return true;
    }

    // 检查是否为好友
    bool isFriend(uint64_t user_id, uint64_t friend_id) {
        std::string sql = "SELECT * FROM friendships WHERE status = 1 AND ("
                        "(user_id = " + std::to_string(user_id) + 
                        " AND friend_id = " + std::to_string(friend_id) + 
                        ") OR (user_id = " + std::to_string(friend_id) + 
                        " AND friend_id = " + std::to_string(user_id) + 
                        "))";
        
        LOG_DEBUG << "isFriend SQL: " << sql;
        
        std::vector<std::map<std::string, std::string>> result;
        if (!executeQuery(sql, result)) {
            LOG_ERROR << "isFriend: executeQuery failed";
            return false;
        }
        
        bool is_friend = !result.empty();
        LOG_DEBUG << "isFriend: " << user_id << " and " << friend_id 
                << " are " << (is_friend ? "" : "not ") << "friends";
        return is_friend;
    }

    // 发送好友请求
    bool sendFriendRequest(uint64_t from_uid, uint64_t to_uid, const std::string& message, uint64_t& request_id) {
        if (!userExists(to_uid)) {
            LOG_ERROR << "Target user does not exist: " << to_uid;
            return false;
        }
        if (!userExists(from_uid)) {
            LOG_ERROR << "Source user does not exist: " << from_uid;
            return false;
        }
        
        // 检查是否已有待处理的请求
        std::string sql1 = "SELECT * FROM friend_requests WHERE from_uid = " + std::to_string(from_uid) + 
                        " AND to_uid = " + std::to_string(to_uid) + " AND status = 0";
        std::vector<std::map<std::string, std::string>> result;
        if (!executeQuery(sql1, result) || !result.empty()) {
            LOG_WARN << "Already has pending request from " << from_uid << " to " << to_uid;
            return false;
        }

        int64_t now = tool::getTimestamp();
        std::string sql2 = "INSERT INTO friend_requests (from_uid, to_uid, message, status, created_at, updated_at) "
                        "VALUES (" + std::to_string(from_uid) + ", " + std::to_string(to_uid) + ", '" + 
                        escapeString(message) + "', 0, " + std::to_string(now) + ", " + std::to_string(now) + ")";
        
        LOG_INFO << "sendFriendRequest: SQL=" << sql2;
        
        // ✅ 修复：执行 sql2（INSERT 语句）
        if (!executeUpdate(sql2)) {
            LOG_ERROR << "Failed to insert friend request";
            return false;
        }

        // 使用 getLastInsertID() 获取 ID
        request_id = getLastInsertID();
        LOG_INFO << "sendFriendRequest: generated request_id=" << request_id;

        return true;
    }

    // 处理好友请求
    bool processFriendRequest(uint64_t request_id, bool accept) {
        TransactionGuard tx(*this);
        int status = accept ? 1 : 2;
        std::string sql = "UPDATE friend_requests SET status = " + std::to_string(status) + 
                           ", updated_at = " + std::to_string(tool::getTimestamp()) + 
                           " WHERE request_id = " + std::to_string(request_id);
        if(!executeUpdate(sql)) {
            return false;
        }
        if(accept) {
            std::string query = "SELECT from_uid, to_uid FROM friend_requests WHERE request_id = " + std::to_string(request_id);
            std::vector<std::map<std::string, std::string>> result;
            if(!executeQuery(query, result) || result.empty()) {
                return false;
            }
            uint64_t from = std::stoull(result[0]["from_uid"]);
            uint64_t to = std::stoull(result[0]["to_uid"]);
            if(!addFriendshipNoTx(from, to)) {
                return false;
            }
        }
        tx.commit();
        return true;
    }

    std::vector<FriendRequestInfo> getPendingRequestsForUser(uint64_t user_id) {
        std::vector<FriendRequestInfo> requests;
        std::string sql = "SELECT request_id, from_uid, to_uid, message, status, created_at "
                        "FROM friend_requests WHERE to_uid = " + std::to_string(user_id) + 
                        " AND status = 0 ORDER BY created_at DESC";
        
        std::vector<std::map<std::string, std::string>> result;
        if (!executeQuery(sql, result)) {
            return requests;
        }
        
        for (const auto& row : result) {
            FriendRequestInfo info;
            info.request_id = std::stoull(row.at("request_id"));
            info.from_uid = std::stoull(row.at("from_uid"));
            info.to_uid = std::stoull(row.at("to_uid"));
            info.message = row.at("message");
            info.status = std::stoi(row.at("status"));
            info.created_at = std::stoll(row.at("created_at"));
            requests.push_back(info);
        }
        return requests;
    }

    // 发出的请求
    std::vector<FriendRequest> getFriendRequest(uint64_t from_uid) {
        std::vector<FriendRequest> request;
        std::string sql = "SELECT * FROM friend_requests WHERE from_uid = " + std::to_string(from_uid) + 
                          " ORDER BY created_at DESC";
        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result)) {
            return request;
        }
        for(const auto& row : result) {
            FriendRequest req;
            fillRequestFromMap(row, req);
            request.push_back(req);
        }
        return request;

    }

    bool getRequestInfo(uint64_t request_id, FriendRequestInfo& info) {
        std::string sql = "SELECT request_id, from_uid, to_uid, message, status, created_at "
                        "FROM friend_requests WHERE request_id = " + std::to_string(request_id);
        
        std::vector<std::map<std::string, std::string>> result;
        if (!executeQuery(sql, result) || result.empty()) {
            return false;
        }
        
        const auto& row = result[0];
        info.request_id = std::stoull(row.at("request_id"));
        info.from_uid = std::stoull(row.at("from_uid"));
        info.to_uid = std::stoull(row.at("to_uid"));
        info.message = row.at("message");
        info.status = std::stoi(row.at("status"));
        info.created_at = std::stoll(row.at("created_at"));
        return true;
    }

    bool deleteAllFriend(uint64_t user_id) {
        char sql[512];
        snprintf(sql, sizeof(sql), "DELETE FROM friendships WHERE user_id = %lu OR frined_id = %lu", user_id, user_id);
        return executeUpdate(sql);
    }

    bool userExists(uint64_t user_id) {
        std::string sql = "SELECT 1 FROM users WHERE user_id = " + std::to_string(user_id);
        std::vector<std::map<std::string, std::string>> result;
        return executeQuery(sql, result) && !result.empty();
    }

private:
    bool addFriendshipNoTx(uint64_t user_id, uint64_t friend_id, const std::string& remark = "") {
        int64_t now = tool::getTimestamp();
        
        // 检查是否已存在关系，避免重复插入
        std::string check_sql1 = "SELECT * FROM friendships WHERE user_id = " + 
                                std::to_string(user_id) + " AND friend_id = " + std::to_string(friend_id);
        std::vector<std::map<std::string, std::string>> result1;
        if (!executeQuery(check_sql1, result1) || result1.empty()) {
            std::string sql1 = "INSERT INTO friendships (user_id, friend_id, status, remark, group_name, created_at, updated_at)" 
                            "VALUES (" + std::to_string(user_id) + ", " + std::to_string(friend_id) + 
                            ", 1, '" + escapeString(remark) + "', '默认', " + 
                            std::to_string(now) + ", " + std::to_string(now) + ")";
            if (!executeUpdate(sql1)) {
                LOG_ERROR << "addFriendshipNoTx: failed to insert first direction";
                return false;
            }
        }
        
        // 检查反向关系
        std::string check_sql2 = "SELECT * FROM friendships WHERE user_id = " + 
                                std::to_string(friend_id) + " AND friend_id = " + std::to_string(user_id);
        std::vector<std::map<std::string, std::string>> result2;
        if (!executeQuery(check_sql2, result2) || result2.empty()) {
            std::string sql2 = "INSERT INTO friendships (user_id, friend_id, status, remark, group_name, created_at, updated_at)" 
                            "VALUES (" + std::to_string(friend_id) + ", " + std::to_string(user_id) + 
                            ", 1, '', '默认', " + std::to_string(now) + ", " + std::to_string(now) + ")";
            if (!executeUpdate(sql2)) {
                LOG_ERROR << "addFriendshipNoTx: failed to insert second direction";
                return false;
            }
        }
        
        LOG_INFO << "addFriendshipNoTx: friendship established between " << user_id << " and " << friend_id;
        return true;
    }


    void fillFriendshipFromMap(const std::map<std::string, std::string>& row, FriendShip& f) {
        f.friendship_id = std::stoull(row.at("friendship_id"));
        f.user_id = std::stoull(row.at("user_id"));
        f.friend_id = std::stoull(row.at("friend_id"));
        f.status = std::stoi(row.at("status"));
        f.remark = row.at("remark");
        f.group_name = row.at("group_name");
        f.created_at = std::stoll(row.at("created_at"));
        f.updated_at = std::stoll(row.at("updated_at"));
    }
    
    void fillRequestFromMap(const std::map<std::string, std::string>& row, FriendRequest& req) {
        req.request_id = std::stoull(row.at("request_id"));
        req.from_uid = std::stoull(row.at("from_uid"));
        req.to_uid = std::stoull(row.at("to_uid"));
        req.message = row.at("message");
        req.status = std::stoi(row.at("status"));
        req.created_at = std::stoll(row.at("created_at"));
        req.updated_at = std::stoll(row.at("updated_at"));
    }
};