#pragma once

#include "baseDAO.h"
#include "../logging.h"
#include "../tool.h"
#include <string>
#include <vector>
#include <map>
#include <queue>
#include <deque>

// 在线消息
struct Message {
    uint64_t msg_id = 0;
    uint64_t from_uid = 0;
    uint64_t to_uid = 0;
    int chat_type = 1;  // 1.私聊  2.群聊
    int msg_type = 1;   // 1.文本  2.图片  3.文件  4.语音  5.视频  6.系统消息
    std::string content;
    std::string extra;
    int status = 0;     // 0.已经发送  1.已经送达  2.已读  3.撤回  4.已删除
    bool is_recalled = false;
    int64_t recalled_at = 0;
    int64_t created_at = 0;
    int64_t delivered_at = 0;
    int64_t read_at = 0;
};


// 离线消息
struct offlineMessage {
    uint64_t offline_id = 0;
    uint64_t user_id = 0;
    uint64_t msg_id = 0;
    int64_t received_at = 0;
    bool is_delivered = false;
    int64_t delivered_at = 0;
};

// 会话对象
struct ConversationInfo {
    uint64_t user_id = 0;
    std::string username;
    std::string nickname;
    std::string avatar;
    uint64_t last_msg_id = 0;
    std::string last_msg_conten;
    int64_t last_msg_time = 0;
    int unread_count = 0;
    int online_status = 0;  // 0.离线  1.在线  2.忙碌  3.离开
};

class MessageDAO : public BaseDAO {
public:
    // 消息存储
    bool saveMessage(const Message& msg, uint64_t& msg_id) {
        std::string sql = "INSERT INTO messages (from_uid, to_uid, chat_type, msg_type, content, extra, "
                          "status, is_recalled, created_at, delivered_at, read_at) VALUES (";

        sql += std::to_string(msg.from_uid) + ", ";
        sql += std::to_string(msg.to_uid) + ", ";
        sql += std::to_string(msg.chat_type) + ", ";
        sql += std::to_string(msg.msg_type) + ", '";
        sql += escapeString(msg.content) + "', '";
        sql += escapeString(msg.extra) + "', ";
        sql += std::to_string(msg.status) + ", ";
        sql += std::to_string(msg.is_recalled ? 1 : 0) + ", ";
        sql += std::to_string(msg.created_at) + ", ";
        sql += std::to_string(msg.delivered_at) + ", ";
        sql += std::to_string(msg.read_at) + ")";
    
        if(!executeUpdate(sql)) {
            LOG_ERROR << "Save message failed";
            return false;
        }

        msg_id = getLastInserterID();
        LOG_DEBUG << "Message saved: " << msg_id << " from " << msg.from_uid << " to " << msg.to_uid;

        return true;
    }

    // 根据消息ID获取消息
    bool getMessageByID(uint64_t msg_id, Message& msg) {
        std::string sql = "SELECT * FROM messages WHERE msg_id = " + std::to_string(msg_id);

        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result)) {
            return false;
        }

        fillMessageFromMap(result[0], msg);
        return true;
    }

    // 历史消息查询（最新消息）
    std::vector<Message> getChatHistory(uint64_t user1_id, uint64_t user2_id, int limit = 100, int64_t before_time = 0) {
        std::vector<Message> messages;

        std::string sql = "SELECT * FROM messages WHERE chat_type = 1 AND ("
                          "(from_uid = " + std::to_string(user1_id) + " AND to_uid = " + std::to_string(user2_id) + 
                          ") OR " "(from_uid = " + std::to_string(user2_id) + " AND to_uid = " + std::to_string(user1_id) + "))";
        if(before_time > 0) {
            sql += " AND created_at < " + std::to_string(before_time);
        }

        sql += " ORDER BY created_at DESC LIMIT " + std::to_string(limit);

        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result)) {
            return messages;
        }

        for(const auto& row : result) {
            Message msg;
            fillMessageFromMap(row, msg);
            messages.push_back(msg);
        }

        // 反转保持时间顺序
        std::reverse(messages.begin(), messages.end());
        return messages;
    }

    // 查询历史消息（较早的消息）
    std::vector<Message> getCharHistoryAfter(uint64_t user1_id, uint64_t user2_id, int64_t after_time, int limit = 100) {
        std::vector<Message> messages;

        std::string sql = "SELECT * FROM messages WHERE chat_type = 1 AND ("
                          "(from_uid = " + std::to_string(user1_id) + " AND to_uid = " + std::to_string(user2_id) + ") OR "
                          "(from_uid = " + std::to_string(user2_id) + " AND to_uid = " + std::to_string(user1_id) + "))"
                          " AND created_at > " + std::to_string(after_time) + 
                          " ORDER BY created_at ASC LIMIT " + std::to_string(limit);
        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result)) {
            return messages;
        }

        for(const auto& row : result) {
            Message msg;
            fillMessageFromMap(row, msg);
            messages.push_back(msg);
        }

        return messages;
    }

    // 单条消息状态更新
    bool updateMessageStatus(uint64_t msg_id, int status) {
        std::string sql = "UPDATE messages SET status = " + std::to_string(status);
        if(status == 1) {
            sql += ", delivered_at = " + std::to_string(tool::getTimestamp()); 
        }
        else if(status == 2) {
            sql += ", read_at = " + std::to_string(tool::getTimestamp());
        }

        sql += " WHERE msg_id = " + std::to_string(msg_id);
        return executeUpdate(sql);
    }

    // 标记所有的消息为已送达
    bool markmessageAsdelivered(uint64_t to_uid, uint64_t from_uid) {
        std::string sql = "UPDATE messages SET status = 1. delivered_at = " + std::to_string(tool::getTimestamp());
                          " WHERE to_uid = " + std::to_string(to_uid);
                          " AND from_uid = " + std::to_string(from_uid);
                          " AND status = 0";
        return executeUpdate(sql);
    }

    // 标记与某人的消息已读
    bool markMessageAsRead(uint64_t to_uid, uint64_t from_uid) {
        std::string sql = "UPDATE messages SET status = 2, read_at = " + std::to_string(tool::getTimestamp()) + 
                          " WHERE to_uid = " + std::to_string(to_uid) +
                          " AND from_uid = " + std::to_string(from_uid) + 
                          " AND status IN (0, 1)";

        return executeUpdate(sql);
    }

    // 标记所有的信息为已读
    bool markAllMessagesAsRead(uint64_t to_uid) {
        std::string sql = "UPDATE messages SET status = 2. read_at = " + std::to_string(tool::getTimestamp());
                          " WHERE to_uid = " + std::to_string(to_uid) + 
                          " AND status IN (0, 1)";

        return executeUpdate(sql);
    }

    // 撤回消息
    bool recallMessage(uint64_t msg_id, uint64_t user_id) {
        std::string sql = "UPDATE messages SET is_recalled = 1, recalled_at = " + std::to_string(tool::getTimestamp()) + 
                          " WHERE msg_id = " + std::to_string(msg_id) + 
                          " AND from_uid = " + std::to_string(user_id) + 
                          " AND is_recalled = 0";

        return executeUpdate(sql);
    }
    
    // 获取用户未读消息的数量
    int getUnreadCount(uint64_t to_uid, uint64_t from_uid = 0) {
        std::string sql = "SELECT COUNT(*) as count FROM messages WHERE to_uid = " + std::to_string(to_uid) + " AND status IN (0, 1)";

        if(from_uid > 0) {
            sql += " AND from_uid = " + std::to_string(from_uid);
        }

        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result) || result.empty()) {
            return 0;
        }

        return std::stoi(result[0]["count"]);
    }

    // 获取每个会话的未读数量
    std::map<uint64_t, int> getUnreadCountByUser(uint64_t to_uid) {
        std::map<uint64_t, int> result_map;

        // 分组聚合查询
        std::string sql = "SELECT from_uid, COUNT(*) as count FROM message "
                          "WHERE to_uid = " + std::to_string(to_uid) + 
                          " AND status IN (0, 1) GROUP BY from_uid";

        // 执行查询
        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result)) {
            return result_map;
        }

        // 填充结果
        for(const auto& row : result) {
            uint64_t from_uid = std::stoull(row.at("from_uid"));
            int count = std::stoi(row.at("count"));
            result_map[from_uid] = count;
        }

        return result_map;
    }

    // 保存离线消息
    bool saveOfflineMessage(uint64_t user_id, uint64_t msg_id) {
        std::string sql = "INSERT INTO offline_messages (user_id, msg_id, received_at, is_delivered)"
                          "VALUES (" + std::to_string(user_id) + ", " + 
                          std::to_string(msg_id) + ", " + 
                          std::to_string(tool::getTimestamp()) + ", 0)";

        return executeUpdate(sql);
    }

    // 获取离线消息
    std::vector<Message> getOfflineMessage(uint64_t user_id) {
        std::vector<Message> messages;
        
        // 查询未投递的离线消息
        std::string sql = "SELECT m.* FROM messages m "
                          "JOIN offline_messages om ON om.msg_id = m.msg_id "
                          "WHERE om.user_id = " + std::to_string(user_id) + 
                          " AND om.is_delivered = 0 "
                          "ORDER BY om.received_at ASC";

        // 执行查询
        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result)) {
            return messages;
        }

        // 填充消息类型
        for(const auto& row : result) {
            Message msg;
            fillMessageFromMap(row, msg);
            messages.push_back(msg);
        }

        // 标记为已投递
        if(!messages.empty()) {
            std::string update_sql = "UPDATE offline_messages SET is_delivered = 1, delivered_at = " + std::to_string(tool::getTimestamp()) + 
                                     " WHERE user_id = " + std::to_string(user_id) + " AND is_delivered = 0";
            executeUpdate(update_sql);
            
            // 更新消息类型为已送达
            for(const auto& msg : messages) {
                updateMessageStatus(msg.msg_id, 1);
            }
        }

        LOG_INFO << "Received " << messages.size() << " offline messages for user " << user_id;
        return messages;
    }

    // 检查是否还有没有投递的离线消息
    bool hasOfflineMessage(uint64_t user_id) {
        std::string sql = "SELECT COUNT(*) as count FROM offline_messages "
                          "WHERE user_id = " + std::to_string(user_id) + 
                          " AND is_delivered = 0";

        // 查询
        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result) || result.empty()) {
            return false;
        }

        // 返回是否 > 0
        return std::stoi(result[0]["count"]) > 0;
    }

    // 获取用户未投递的消息的数量
    int getOfflineMessageCount(uint64_t user_id) {
        std::string sql = "SELECT COUNT(*) as count FROM offline_messages "
                          "WHERE user_id = " + std::to_string(user_id) + 
                          " AND is_delivered = 0";

        // 执行查询
        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result) || result.empty()) {
            return 0;
        }

        // 返回数量
        return std::stoi(result[0]["count"]);
    }

    // 清理过期的消息(7天前)
    bool cleanOfflineMessages(int day = 7) {
        int64_t cur_time = tool::getTimestamp() - day * 24 * 3600 * 1000;
        std::string  sql = "DELETE FROM dffline_messages WHERE received_at < " + std::to_string(cur_time); 
        return executeUpdate(sql);
    }


private:
    void fillMessageFromMap(const std::map<std::string, std::string>& row, Message& msg) {
        msg.msg_id = std::stoull(row.at("msg_id"));
        msg.msg_type = std::stoi(row.at("msg_type"));
        msg.from_uid = std::stoull(row.at("from_uid"));
        msg.to_uid = std::stoull(row.at("to_usd"));
        msg.chat_type = std::stoi(row.at("chat_type"));
        msg.content = row.at("content");
        msg.extra = row.at("extra");
        msg.status = std::stoi(row.at("status"));
        msg.is_recalled = std::stoll(row.at("is_recalled")) == 1;
        msg.created_at = std::stoll(row.at("created_at"));
        msg.delivered_at = std::stoll(row.at("delivered_at"));
        msg.read_at = std::stoll(row.at("read_at"));
    }
};