#pragma once

#include "baseDAO.h"
#include "tool/logging.h"
#include "tool/tool.h"
#include <string>
#include <vector>
#include <map>
#include <queue>
#include <deque>
#include "protobuf/mysql_p.h"


// 在线消息
struct Message {
    uint64_t msg_id = 0;
    uint64_t from_uid = 0;
    uint64_t to_uid = 0;
    int chat_type = 1;  // 1.私聊  2.群聊
    int msg_type = 1;   // 1.文本  2.图片  3.文件  4.语音  5.视频  6.系统消息
    std::string content;
    std::string extra;  // 存储序列化的MessageExtra
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
    std::string last_msg_content;
    int64_t last_msg_time = 0;
    int unread_count = 0;
    int online_status = 0;  // 0.离线  1.在线  2.忙碌  3.离开
};

class MessageDAO : public BaseDAO {
public:
    // 消息存储
    bool saveMessage(const Message& msg, uint64_t& msg_id) {
        std::string extra_json = msg.extra.empty() ? "{}" : msg.extra;
        std::string sql = "INSERT INTO messages (from_uid, to_uid, chat_type, msg_type, content, extra, "
                          "status, is_recalled, created_at, delivered_at, read_at) VALUES (";

        sql += std::to_string(msg.from_uid) + ", ";
        sql += std::to_string(msg.to_uid) + ", ";
        sql += std::to_string(msg.chat_type) + ", ";
        sql += std::to_string(msg.msg_type) + ", '";
        sql += escapeString(msg.content) + "', '";
        sql += escapeString(extra_json) + "', ";
        sql += std::to_string(msg.status) + ", ";
        sql += std::to_string(msg.is_recalled ? 1 : 0) + ", ";
        sql += std::to_string(msg.created_at) + ", ";
        sql += std::to_string(msg.delivered_at) + ", ";
        sql += std::to_string(msg.read_at) + ")";
    
        if(!executeUpdate(sql)) {
            LOG_ERROR << "Save message failed";
            return false;
        }

        msg_id = getLastInsertID();
        LOG_DEBUG << "Message saved: " << msg_id << " from " << msg.from_uid << " to " << msg.to_uid;

        return true;
    }

    // 使用Protobuf存储
    bool saveMessage(const Message& msg, const db::MessageExtra& extra, uint64_t& msg_id) {
        Message msg_copy = msg;
        msg_copy.extra = Switch::sToJson(extra);
        return saveMessage(msg_copy, msg_id);
    }

    // 获取消息并解析为Protobuf
    bool getMessageById(uint64_t msg_id, Message& msg, db::MessageExtra& extra) {
        if (!getMessageByID(msg_id, msg)) {
            return false;
        }
        return Switch::dsFromJson(msg.extra, extra);
    }

    // 获取历史消息并解析extra
    std::vector<std::pair<Message, db::MessageExtra>> getChatHistoryWithExtra(
        uint64_t user1_id, uint64_t user2_id, int limit = 100) {
        std::vector<std::pair<Message, db::MessageExtra>> results;
        auto messages = getChatHistory(user1_id, user2_id, limit);
        
        for (const auto& msg : messages) {
            db::MessageExtra extra;
            Switch::dsFromJson(msg.extra, extra);
            results.push_back({msg, extra});
        }
        return results;
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
                          ") OR " "(from_uid = " + std::to_string(user2_id) + " AND to_uid = " + std::to_string(user1_id) + "))"
                          " AND is_recalled = 0";
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
    std::vector<Message> getChatHistoryAfter(uint64_t user1_id, uint64_t user2_id, int64_t after_time, int limit = 100) {
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
        std::string sql = "UPDATE messages SET status = 1, delivered_at = " + std::to_string(tool::getTimestamp());
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
        std::string sql = "UPDATE messages SET status = 2, read_at = " + std::to_string(tool::getTimestamp()) +
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
        std::string sql = "SELECT from_uid, COUNT(*) as count FROM messages "
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

    bool deleteAllOfflineMessages(uint64_t user_id) {
        char sql[512];
        snprintf(sql, sizeof(sql),
            "DELETE FROM offline_messages WHERE user_id = %lu", user_id);
        return executeUpdate(sql);
    }

    // 获取离线消息
    std::vector<Message> getOfflineMessages(uint64_t user_id) {
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

    // 清理过期的离线消息(7天前)
    bool cleanOfflineMessages(int day = 7) {
        int64_t cur_time = tool::getTimestamp() - day * 24 * 3600 * 1000;
        std::string  sql = "DELETE FROM offline_messages WHERE received_at < " + std::to_string(cur_time); 
        return executeUpdate(sql);
    }

    // 清理过期的消息(一个月前)
    bool cleanOldMessages(int day = 30) {
        int64_t cutoff_time = tool::getTimestamp() - day * 24 * 3600 * 1000;
        std::string sql = "DELETE FROM messages WHERE created_at < " + std::to_string(cutoff_time) + " AND status = 4";
        return executeUpdate(sql); 
    }

    // 会话列表
    std::vector<ConversationInfo> getConversationList(uint64_t user_id, int limit = 50) {
        std::vector<ConversationInfo> conversations;

        // 获取最近的会话
        std::string sql = "SELECT CASE WHEN from_uid = " + std::to_string(user_id) + " THEN to_uid ELSE from_uid END as other_uid, "
                          "MAX(msg_id) as last_msg_id, " "MAX(created_at) as last_time "
                          "FROM messages WHERE (from_uid = " + std::to_string(user_id) + 
                          " OR to_uid = " + std::to_string(user_id) + ") " "AND chat_type = 1 "
                          "GROUP BY other_uid "
                          "ORDER BY last_time DESC LIMIT " + std::to_string(limit);
        
        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result)) {
            return conversations;
        }

        for(const auto& row : result) {
            ConversationInfo info;
            auto it_other = row.find("other_uid");
            if (it_other == row.end()) 
                continue;
            uint64_t other_uid = std::stoull(it_other->second);
            info.user_id = other_uid;

            auto it_last_msg = row.find("last_msg_id");
            info.last_msg_id = (it_last_msg != row.end() && !it_last_msg->second.empty()) 
                               ? std::stoull(it_last_msg->second) : 0;
            
            auto it_last_time = row.find("last_time");
            info.last_msg_time = (it_last_time != row.end() && !it_last_time->second.empty()) ? std::stoll(it_last_time->second) : 0;
        
            // 获取用户的信息
            std::string user_sql = "SELECT username, nickname, avatar, status FROM users WHERE user_id = " +std::to_string(other_uid);
            std::vector<std::map<std::string, std::string>> user_result;
            if(executeQuery(user_sql, user_result) && !user_result.empty()) {
                info.username = user_result[0]["username"];
                info.nickname = user_result[0]["nickname"];
                info.avatar = user_result[0]["avatar"];
                info.online_status = std::stoi(user_result[0]["status"]);
            }

            // 获取最后一条消息内容
            // std::string msg_sql = "SELECT content, msg_type FROM messages WHERE msg_id = " + std::to_string(info.last_msg_id);
            // std::vector<std::map<std::string, std::string>> msg_result;
            // if(executeQuery(msg_sql, msg_result) && ! msg_result.empty()) {
            //     int msg_type = std::stoi(msg_result[0]["msg_type"]);
            //     if(msg_type == 1) {
            //         info.last_msg_content = msg_result[0]["content"];
            //     }
            //     else {
            //         info.last_msg_content = "[" +  getMessageTypeName(msg_type) + "]";
            //     }
            // }

            // 获取未读的数量
            info.unread_count = getUnreadCount(user_id, other_uid);

            conversations.push_back(info);
        }

        return conversations;
    }

    // 搜索聊天记录
    std::vector<Message> searchMessage(uint64_t user_id, const std::string& keyword, int limit = 50, int64_t before_time = 0) {
        std::vector<Message> messages;

        // 查询条件
        std::string sql = "SELECT * FROM messages WHERE (from_uid = " + std::to_string(user_id) + 
                          " OR to_uid = " + std::to_string(user_id) + ") "
                          "AND content LIKE '%" + escapeString(keyword) + "%' ";
        
        // 分页参数
        if(before_time > 0) {
            sql += " AND created_at < " + std::to_string(before_time);
        }

        // 排序
        sql += " ORDER BY created_at DESC LIMIT " + std::to_string(limit);\

        // 执行查询
        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result)) {
            return messages;
        }

        // 填充结果
        for(const auto& row : result) {
            Message msg;
            fillMessageFromMap(row, msg);
            messages.push_back(msg);
        }

        // 升序
        std::reverse(messages.begin(), messages.end());
        return messages;
    }

    bool deleteAllMessage(uint64_t user_id) {
        char sql[512];
        snprintf(sql, sizeof(sql), "DELETE FROM messages WHERE from_uid = %lu OR to_uid = %lu", user_id, user_id);
        return executeUpdate(sql);
    }

private:
    void fillMessageFromMap(const std::map<std::string, std::string>& row, Message& msg) {
        msg.msg_id = std::stoull(row.at("msg_id"));
        msg.msg_type = std::stoi(row.at("msg_type"));
        msg.from_uid = std::stoull(row.at("from_uid"));
        msg.to_uid = std::stoull(row.at("to_uid"));
        msg.chat_type = std::stoi(row.at("chat_type"));
        msg.content = row.at("content");
        msg.extra = row.at("extra");
        msg.status = std::stoi(row.at("status"));
        msg.is_recalled = std::stoll(row.at("is_recalled")) == 1;
        msg.created_at = std::stoll(row.at("created_at"));
        msg.delivered_at = std::stoll(row.at("delivered_at"));
        msg.read_at = std::stoll(row.at("read_at"));

        auto it_delivered = row.find("delivered_at");
        msg.delivered_at = (it_delivered != row.end() && !it_delivered->second.empty()) 
                        ? std::stoll(it_delivered->second) : 0;
        
        auto it_read = row.find("read_at");
        msg.read_at = (it_read != row.end() && !it_read->second.empty()) 
                    ? std::stoll(it_read->second) : 0;

        auto it_recalled = row.find("recalled_at");
        if (it_recalled != row.end() && !it_recalled->second.empty()) {
            msg.recalled_at = std::stoll(it_recalled->second);
        } else {
            msg.recalled_at = 0;
        }
    }

    std::string getMessageTypeName(int type) {
        switch(type) {
            case 2:
                return "图片";
            case 3:
                return "文件";
            case 4:
                return "语音";
            case 5:
                return "视频";
            case 6:
                return "系统消息";
            default:
                return "未知";
        }
    }
};