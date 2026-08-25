#pragma once
#include "mysql/baseDAO.h"
#include "tool/logging.h"
#include "tool/tool.h"
#include "protobuf/mysql_p.h"
#include <string>
#include <vector>
#include <map>

struct GroupMessage {
    uint64_t msg_id = 0;
    uint64_t group_id = 0;
    uint64_t from_uid = 0;
    std::string from_username;
    std::string from_nickname;
    int msg_type = 1;
    std::string content;
    std::string extra;
    int status = 0;
    bool is_recalled = false;
    int64_t recalled_at = 0;
    int64_t created_at = 0;
};

struct GroupOfflineMessage {
    uint64_t offline_id = 0;
    uint64_t group_id = 0;
    uint64_t user_id = 0;
    uint64_t msg_id = 0;
    int64_t received_at = 0;
    bool is_delivered = false;
    int64_t delivered_at = 0;
};

struct GroupLastRead {
    uint64_t user_id = 0;
    uint64_t group_id = 0;
    uint64_t last_msg_id = 0;
    int64_t last_read_time = 0;
    int unread_count = 0;
};

class GroupMessageDAO : public BaseDAO {
public:
    // 存储群消息    
    bool saveGroupMessage(const GroupMessage& msg, uint64_t& msg_id) {
        TransactionGuard tx(*this);
        // 构建extra
        db::GroupMessageExtra extra;

        extra.set_group_id(msg.group_id);
        extra.set_from_uid(msg.from_uid);
        extra.set_msg_type(msg.msg_type);
        extra.set_content(msg.content);
        extra.set_created_at(msg.created_at);
        extra.set_is_recalled(msg.is_recalled);
        extra.set_recalled_at(msg.recalled_at);
        extra.set_status(msg.status);

        if (!msg.extra.empty()) {
            db::GroupMessageExtra old_extra;
            if (Switch::dsFromJson(msg.extra, old_extra)) {
                // 合并数据
                for (const auto& [key, value] : old_extra.metadata()) {
                    extra.mutable_metadata()->insert({key, value});
                }
            }
        }
        
        std::string extra_json = Switch::sToJson(extra);
        
        std::string sql = "INSERT INTO group_messages (group_id, from_uid, msg_type, content, extra, "
                         "status, is_recalled, created_at) VALUES (";
        sql += std::to_string(msg.group_id) + ", ";
        sql += std::to_string(msg.from_uid) + ", ";
        sql += std::to_string(msg.msg_type) + ", '";
        sql += escapeString(msg.content) + "', '";
        sql += escapeString(extra_json) + "', ";
        sql += std::to_string(msg.status) + ", ";
        sql += std::to_string(msg.is_recalled ? 1 : 0) + ", ";
        sql += std::to_string(msg.created_at) + ")";
        
        if (!executeUpdate(sql)) {
            LOG_ERROR << "Save group message failed";
            return false;
        }
        
        msg_id = getLastInsertID();
        LOG_DEBUG << "Group message saved: " << msg_id << " in group " << msg.group_id;
        tx.commit();
        return true;
    }
    
    bool getGroupMessageById(uint64_t msg_id, GroupMessage& msg) {
        std::string sql = "SELECT gm.*, u.username, u.nickname FROM group_messages gm "
                         "LEFT JOIN users u ON gm.from_uid = u.user_id "
                         "WHERE gm.msg_id = " + std::to_string(msg_id);
        
        std::vector<std::map<std::string, std::string>> result;
        if (!executeQuery(sql, result) || result.empty()) {
            return false;
        }
        
        fillGroupMessageFromMap(result[0], msg);
        return true;
    }
    
    // 群历史消息
    std::vector<GroupMessage> getGroupHistory(uint64_t group_id, int limit = 100, int64_t before_time = 0) {
        std::vector<GroupMessage> messages;
        
        std::string sql = "SELECT gm.*, u.username, u.nickname FROM group_messages gm "
                         "LEFT JOIN users u ON gm.from_uid = u.user_id "
                         "WHERE gm.group_id = " + std::to_string(group_id) + 
                         " AND gm.is_recalled = 0 ";
        
        if (before_time > 0) {
            sql += " AND gm.created_at < " + std::to_string(before_time);
        }
        
        sql += " ORDER BY gm.created_at DESC LIMIT " + std::to_string(limit);
        
        std::vector<std::map<std::string, std::string>> result;
        if (!executeQuery(sql, result)) {
            return messages;
        }
        
        for (const auto& row : result) {
            GroupMessage msg;
            fillGroupMessageFromMap(row, msg);
            messages.push_back(msg);
        }
        
        std::reverse(messages.begin(), messages.end());
        return messages;
    }
    
    std::vector<GroupMessage> getGroupHistoryAfter(uint64_t group_id, int64_t after_time, int limit = 100) {
        std::vector<GroupMessage> messages;
        
        std::string sql = "SELECT gm.*, u.username, u.nickname FROM group_messages gm "
                         "LEFT JOIN users u ON gm.from_uid = u.user_id "
                         "WHERE gm.group_id = " + std::to_string(group_id) + 
                         " AND gm.created_at > " + std::to_string(after_time) +
                         " AND gm.is_recalled = 0 "
                         " ORDER BY gm.created_at ASC LIMIT " + std::to_string(limit);
        
        std::vector<std::map<std::string, std::string>> result;
        if (!executeQuery(sql, result)) {
            return messages;
        }
        
        for (const auto& row : result) {
            GroupMessage msg;
            fillGroupMessageFromMap(row, msg);
            messages.push_back(msg);
        }
        
        return messages;
    }
    
    // 群离线消息    
    bool saveGroupOfflineMessage(uint64_t user_id, uint64_t group_id, uint64_t msg_id) {
        std::string sql = "INSERT INTO group_offline_messages (user_id, group_id, msg_id, "
                         "received_at, is_delivered) VALUES (";
        sql += std::to_string(user_id) + ", ";
        sql += std::to_string(group_id) + ", ";
        sql += std::to_string(msg_id) + ", ";
        sql += std::to_string(tool::getTimestamp()) + ", 0)";
        return executeUpdate(sql);
    }

    
    
    std::vector<GroupMessage> getGroupOfflineMessages(uint64_t user_id) {
        std::vector<GroupMessage> messages;
        
        std::string sql = "SELECT gm.*, u.username, u.nickname FROM group_messages gm "
                         "JOIN group_offline_messages gom ON gom.msg_id = gm.msg_id "
                         "LEFT JOIN users u ON gm.from_uid = u.user_id "
                         "WHERE gom.user_id = " + std::to_string(user_id) + 
                         " AND gom.is_delivered = 0 "
                         "ORDER BY gom.received_at ASC";
        
        std::vector<std::map<std::string, std::string>> result;
        if (!executeQuery(sql, result)) {
            LOG_ERROR << "Failed to get group offline messages for user " << user_id;
            return messages;
        }
        
        LOG_DEBUG << "Query returned " << result.size() << " rows for user " << user_id;

        for (const auto& row : result) {
            try {
                GroupMessage msg;
                fillGroupMessageFromMap(row, msg);
                LOG_DEBUG << "Filled msg: from_uid=" << msg.from_uid 
                        << ", content=" << msg.content;
                messages.push_back(msg);
            } catch (const std::exception& e) {
                LOG_ERROR << "Failed to parse row: " << e.what();
                // 继续处理下一行
            }
        }
        
        // 标记为已送达
        if (!messages.empty()) {
            std::string update_sql = "UPDATE group_offline_messages SET is_delivered = 1, "
                                    "delivered_at = " + std::to_string(tool::getTimestamp()) +
                                    " WHERE user_id = " + std::to_string(user_id) + 
                                    " AND is_delivered = 0";
            executeUpdate(update_sql);
        }
        
        LOG_INFO << "Retrieved " << messages.size() << " group offline messages for user " << user_id;
        return messages;
    }
    
    int getGroupOfflineCount(uint64_t user_id) {
        std::string sql = "SELECT COUNT(*) as count FROM group_offline_messages "
                         "WHERE user_id = " + std::to_string(user_id) + 
                         " AND is_delivered = 0";
        
        std::vector<std::map<std::string, std::string>> result;
        if (!executeQuery(sql, result) || result.empty()) {
            return 0;
        }
        
        return std::stoi(result[0]["count"]);
    }
    
    // 消息状态    
    bool updateGroupMessageStatus(uint64_t msg_id, int status) {
        std::string sql = "UPDATE group_messages SET status = " + std::to_string(status) +
                         " WHERE msg_id = " + std::to_string(msg_id);
        return executeUpdate(sql);
    }
    
    bool recallGroupMessage(uint64_t msg_id, uint64_t user_id) {
        std::string sql = "UPDATE group_messages SET is_recalled = 1, recalled_at = " + 
                         std::to_string(tool::getTimestamp()) +
                         " WHERE msg_id = " + std::to_string(msg_id) +
                         " AND from_uid = " + std::to_string(user_id) +
                         " AND is_recalled = 0";
        return executeUpdate(sql);
    }
    
    
    bool updateGroupLastRead(uint64_t user_id, uint64_t group_id, uint64_t msg_id) {
        std::string sql = "INSERT INTO group_read_status (user_id, group_id, last_msg_id, last_read_time) "
                         "VALUES (" + std::to_string(user_id) + ", " + 
                         std::to_string(group_id) + ", " + 
                         std::to_string(msg_id) + ", " +
                         std::to_string(tool::getTimestamp()) + ") "
                         "ON DUPLICATE KEY UPDATE last_msg_id = " + std::to_string(msg_id) + 
                         ", last_read_time = " + std::to_string(tool::getTimestamp());
        return executeUpdate(sql);
    }
    
    GroupLastRead getGroupLastRead(uint64_t user_id, uint64_t group_id) {
        GroupLastRead info;
        info.user_id = user_id;
        info.group_id = group_id;
        
        std::string sql = "SELECT last_msg_id, last_read_time FROM group_read_status "
                         "WHERE user_id = " + std::to_string(user_id) + 
                         " AND group_id = " + std::to_string(group_id);
        
        std::vector<std::map<std::string, std::string>> result;
        if (!executeQuery(sql, result) || result.empty()) {
            return info;
        }
        
        info.last_msg_id = std::stoull(result[0]["last_msg_id"]);
        info.last_read_time = std::stoll(result[0]["last_read_time"]);
        
        // 计算未读数量
        std::string count_sql = "SELECT COUNT(*) as count FROM group_messages "
                               "WHERE group_id = " + std::to_string(group_id) + 
                               " AND msg_id > " + std::to_string(info.last_msg_id);
        
        std::vector<std::map<std::string, std::string>> count_result;
        if (executeQuery(count_sql, count_result) && !count_result.empty()) {
            info.unread_count = std::stoi(count_result[0]["count"]);
        }
        
        return info;
    }
    
    //  清理过期消息
    bool cleanOldGroupMessages(int days = 30) {
        int64_t cutoff_time = tool::getTimestamp() - days * 24 * 3600 * 1000;
        std::string sql = "DELETE FROM group_messages WHERE created_at < " + 
                         std::to_string(cutoff_time) + " AND is_recalled = 1";
        return executeUpdate(sql);
    }
    
    bool cleanGroupOfflineMessages(int days = 7) {
        int64_t cutoff_time = tool::getTimestamp() - days * 24 * 3600 * 1000;
        std::string sql = "DELETE FROM group_offline_messages WHERE received_at < " + 
                         std::to_string(cutoff_time);
        return executeUpdate(sql);
    }

    bool deleteMessagesGroup(uint64_t user_id) {
        char sql[512];
        snprintf(sql, sizeof(sql), "DELETE FROM group_messages WHERE from_uid = %lu", user_id);
        return executeUpdate(sql);
    }
    
private:
    void fillGroupMessageFromMap(const std::map<std::string, std::string>& row, GroupMessage& msg) {
        msg.msg_id = std::stoull(row.at("msg_id"));
        msg.group_id = std::stoull(row.at("group_id"));
        msg.from_uid = std::stoull(row.at("from_uid"));
        if (row.find("username") != row.end()) {
            msg.from_username = row.at("username");
        }
        if (row.find("nickname") != row.end()) {
            msg.from_nickname = row.at("nickname");
        }
        msg.msg_type = std::stoi(row.at("msg_type"));
        msg.content = row.at("content");
        msg.extra = row.at("extra");
        msg.status = std::stoi(row.at("status"));
        msg.is_recalled = std::stoi(row.at("is_recalled")) == 1;
        msg.created_at = std::stoll(row.at("created_at"));

        auto it_recalled = row.find("recalled_at");
        if (it_recalled != row.end() && !it_recalled->second.empty()) {
            msg.recalled_at = std::stoll(it_recalled->second);
        } else {
            msg.recalled_at = 0;
        }
    }
};