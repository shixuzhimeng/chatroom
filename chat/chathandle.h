#pragma once 

#include "protobuf/p.h"
#include "mysql/userDAO.h"
#include "mysql/friendDAO.h"
#include "mysql/messageDAO.h"
#include "mysql/pingbiDAO.h"
#include "mysql/mysqlPool.h"
#include "../logging.h"
#include "../epoll.h"
#include "tool.h"
#include <unordered_map>
#include <map>
#include <mutex>
#include "TLS/TLS.h"
#include "../deduplicator.h"
#include "../Check.h"


class ChatHandle {
public:
    ChatHandle() = default;

    void setUserConnections(std::unordered_map<uint64_t, std::shared_ptr<TcpConnection>>* conns) {
        user_connections_ = conns;
    }

    // 设置好友DAO引用
    void setFriendDAO(FriendDAO* friend_dao) {
        friend_dao_ = friend_dao;
    }

    void setBlockDAO(BlockDAO* block_dao) {
        block_dao_ = block_dao;
    }

    // 私聊消息的处理
    void handleChat(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        // 解析请求
        p::ChatMessage request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendChatResponse(conn, header, false, "Invalid request");
            return ;
        }

        if(!InputValidator::validateMessageContent(request.content())) {
            sendChatResponse(conn, header, false, "invalid message content");
            return ;
        }

        // 验证登录状态
        uint64_t from_uid = conn->getUserID();
        if(from_uid == 0) {
            sendChatResponse(conn, header, false, "User not logged in");
            return ;
        }

        uint64_t to_uid = request.to_uid();
        std::string content = request.content();
        int msg_type = request.msg_type();

        LOG_INFO << "Chat from " << from_uid << " to " << to_uid << " : " << content;

        // 不能给自己发消息
        if (from_uid == to_uid) {
            sendChatResponse(conn, header, false, "Cannot send message to yourself");
            return ;
        }

        // 验证好友关系
        if(friend_dao_ && !friend_dao_->isFriend(to_uid, from_uid)) {
            sendChatResponse(conn, header, false, "Not friend");
            return ;
        }

        // 检查是否被屏蔽
        if(block_dao_ && block_dao_->isBlocked(to_uid, from_uid)){
            sendChatResponse(conn, header, false, "You are blocked by this user");
            return ;
        }

        // 消息保存
        Message msg;
        msg.from_uid = from_uid;
        msg.to_uid = to_uid;
        msg.chat_type = 1;  // 私聊
        msg.msg_type = msg_type > 0 ? msg_type : 1;
        msg.content = content;
        msg.status = 0;
        msg.created_at = tool::getTimestamp();
        msg.delivered_at = 0;
        msg.read_at = 0;

        MessageDAO dao;
        uint64_t msg_id;
        if(!dao.saveMessage(msg, msg_id)) {
            sendChatResponse(conn, header, false, "Failed to save message");
            return ;
        }

        MessageDeduplicator& dedup = MessageDeduplicator::getInstance();
        if(dedup.isDuplicate(msg_id)) {
            LOG_ERROR << "Duplicate message detected, msg_id=" << msg_id;
            return ;
        }
        dedup.markProcessed(msg_id);

        // 回显给发送方
        p::MessageHeader echo_header;
        echo_header.set_msg_type(p::MSG_CHAT);
        echo_header.set_msg_id(msg_id);
        echo_header.set_timestamp(tool::getTimestamp());
        echo_header.set_from_uid(from_uid);
        echo_header.set_to_uid(to_uid);
        
        // 构造回显消息
        p::ChatMessage echo_msg;
        echo_msg.set_from_uid(from_uid);
        echo_msg.set_to_uid(to_uid);
        echo_msg.set_content(content);
        echo_msg.set_msg_type(msg_type);
        echo_msg.set_msg_id(msg_id);
        
        auto echo_data = proto::MessageCodec::encode(echo_header, echo_msg);
        if(!echo_data.empty()) {
            conn->send(echo_data.data(), echo_data.size());
            LOG_DEBUG << "Echo sent to sender " << from_uid;
        }

        // 检查对方是否在线
        bool is_online = false;
        std::shared_ptr<TcpConnection> target_conn;
        if(user_connections_) {
            auto it = user_connections_->find(to_uid);
            if(it != user_connections_->end()) {
                target_conn = it->second;
                is_online = true;
            }
        }

        if(is_online && target_conn) {
            // 在线，推送给接收方
            p::MessageHeader push_header;
            push_header.set_msg_type(p::MSG_CHAT);
            push_header.set_msg_id(msg_id);
            push_header.set_timestamp(tool::getTimestamp());
            push_header.set_from_uid(from_uid);
            push_header.set_to_uid(to_uid);
            
            auto data = proto::MessageCodec::encode(push_header, echo_msg);
            if(!data.empty()) {
                target_conn->send(data.data(), data.size());
                dao.updateMessageStatus(msg_id, 1);
            }

            sendChatResponse(conn, header, true, "Message delivered");
        }
        else {
            // 离线存储消息
            if(dao.saveOfflineMessage(to_uid, msg_id)) {
                sendChatResponse(conn, header, true, "Message saved (offline)");
                LOG_INFO << "Message saved offline for user " << to_uid;
            }
            else {
                sendChatResponse(conn, header, false, "Failed to save offline message");
            }
        }
    }

    // 获取聊天历史
    void handleGethistory(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        // 解析请求
        p::GetHistoryRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendChatResponse(conn, header, false, "Invalid request");
            return ;
        }

        // 验证用户的登录状态
        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendChatResponse(conn, header, false, "User not logged in");
            return ;
        }

        // 提取请求
        uint64_t target_uid = request.target_uid();
        int limit = request.limit() > 0 ? request.limit() : 100;
        int64_t before_time = request.before_time();

        // 查询结果
        MessageDAO dao;
        std::vector<Message> messages;

        if(before_time > 0) {
            messages = dao.getChatHistory(user_id, target_uid, limit, before_time);
        }
        else {
            messages = dao.getChatHistory(user_id, target_uid, limit);
        }

        // 响应
        p::HistoryResponse response;
        response.set_success(true);
        response.set_target_uid(target_uid);

        for(const auto& msg : messages) {
            auto* msg_info = response.add_messages();
            msg_info->set_msg_id(msg.msg_id);
            msg_info->set_msg_type(msg.msg_type);
            msg_info->set_from_uid(msg.from_uid);
            msg_info->set_to_uid(msg.to_uid);
            msg_info->set_content(msg.content);
            msg_info->set_status(msg.status);
            msg_info->set_is_recalled(msg.is_recalled);
            msg_info->set_created_at(msg.created_at);
            msg_info->set_read_at(msg.read_at);
            msg_info->set_delivered_at(msg.delivered_at);
        }

        sendHistoryResponse(conn, header, response);
        LOG_INFO << "Retrieved " << messages.size() << " messages for chat between " << user_id << " and " << target_uid;
    }

    // 标记消息为已读
    void handleMarkRead(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::MarkReadRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendChatResponse(conn, header, false, "Invalid requset");
            return ;
        }

        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendChatResponse(conn, header, false, "User not logged in");
            return ;
        }

        uint64_t from_uid = request.from_uid();
        MessageDAO dao;
        bool result = false;
        
        if (from_uid > 0) {
            result = dao.markMessageAsRead(user_id, from_uid);
        }
        else {
            result = dao.markAllMessagesAsRead(user_id);
        }

        if(result) {
            // 通知消息已读
            if(from_uid > 0 && user_connections_) {
                auto it = user_connections_->find(from_uid);
                if(it != user_connections_->end()) {
                    p::ReadReceipt receipt;
                    receipt.set_user_id(user_id);
                    receipt.set_timestamp(tool::getTimestamp());
                
                    p::MessageHeader receipt_header;
                    receipt_header.set_msg_type(p::MSG_READ_RECEIPT);
                    receipt_header.set_timestamp(tool::getTimestamp());
                    receipt_header.set_from_uid(user_id);
                    receipt_header.set_to_uid(from_uid);

                    auto data = proto::MessageCodec::encode(receipt_header, receipt);
                    if(!data.empty()) {
                        it->second->send(data.data(), data.size());
                    }
                }
            }
            sendChatResponse(conn, header, true, "Message marked as read");
        }
        else {
            sendChatResponse(conn, header, false, "Failed to mark");
        }
    }

    // 获取未读消息的数量
    void handleGetUnreadCount(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendChatResponse(conn, header, false, "User not logged in");
            return ;
        }

        MessageDAO dao;
        auto unread_map = dao.getUnreadCountByUser(user_id);
        int total_unread = 0;

        p::UnreadCountResponse response;
        response.set_success(true);
        response.set_total_unread(0);

        for(const auto& pair : unread_map) {
            uint64_t from_uid = pair.first;
            int count = pair.second;
            auto* item = response.add_unread_items();
            item->set_from_uid(from_uid);
            item->set_count(count);
            total_unread += count;
        }
        response.set_total_unread(total_unread);

        sendUnreadCountResponse(conn, header, response);
        LOG_DEBUG << "Unread count for user " << user_id << ": " << total_unread;
    }

    // 获取会话列表
    void handleGetConversations(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendChatResponse(conn, header, false, "User not logged in");
            return ;
        }

        MessageDAO dao;
        auto conversations = dao.getConversationList(user_id, 50);

        p::ConversationListResponse response;
        response.set_success(true);

        for(const auto& conv : conversations) {
            auto* item = response.add_conversations();
            item->set_user_id(conv.user_id);
            item->set_username(conv.username);
            item->set_nickname(conv.nickname);
            item->set_avatar(conv.avatar);
            item->set_last_msg_content(conv.last_msg_content);
            item->set_last_msg_time(conv.last_msg_time);
            item->set_unread_count(conv.unread_count);
            item->set_online_status(conv.online_status);
        }

        sendConversationListResponse(conn, header, response);
        LOG_DEBUG << "Retrieved " << conversations.size() << "conversations for user " << user_id;
    }

    // 撤回消息
    void handleRecallMessage(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::RecallMessageRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendChatResponse(conn, header, false, "Invalid request");
            return ;
        }

        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendChatResponse(conn, header, false, "User not logged in");
            return ;
        }

        uint64_t msg_id = request.msg_id();

        MessageDAO dao;
        // 获取消息信息
        Message msg;
        if(!dao.getMessageByID(msg_id, msg)) {
            sendChatResponse(conn, header, false, "Mesage not found");
            return ;
        }

        // 只能撤回自己发送的信息
        if(msg.from_uid != user_id) {
            sendChatResponse(conn, header, false, "Cannot recall others message");
            return ;
        }

        // 检查是否超时
        int64_t now = tool::getTimestamp();
        if(now - msg.created_at > 2 * 60 * 1000) {
            sendChatResponse(conn, header, false, "connot recall message after 2 min");
            return ;
        }
        
        if(!dao.recallMessage(msg_id, user_id)) {
            sendChatResponse(conn, header, false, "Failed to recall message");
            return ;
        }

        // 通知消息被撤回
        p::MessageRecall recall;
        recall.set_msg_id(msg_id);
        recall.set_from_uid(user_id);
        recall.set_to_uid(msg.to_uid);
        recall.set_recalled_at(tool::getTimestamp());

        p::MessageHeader recall_header;
        recall_header.set_msg_type(p::MSG_RECALL);
        recall_header.set_timestamp(tool::getTimestamp());
        recall_header.set_from_uid(user_id);
        recall_header.set_to_uid(msg.to_uid);
        recall_header.set_msg_id(msg_id);

        auto recall_data = proto::MessageCodec::encode(recall_header, recall);

        // 通知接收方
        if(user_connections_) {
            auto it = user_connections_->find(msg.to_uid);
            if(it != user_connections_->end()) {
                it->second->send(recall_data.data(), recall_data.size());
                LOG_INFO << "Recall notification sent to receiver " << msg.to_uid;
            }
        }

    // 回显给发送方（自己）
    conn->send(recall_data.data(), recall_data.size());
        
        sendChatResponse(conn, header, true, "Message recalled");
        LOG_INFO << "Message " << msg_id << " recalled by " << user_id;
    }
    
    // 发送离线消息
    void sendOfflineMessage(uint64_t user_id) {
        MessageDAO dao;
        auto messages = dao.getOfflineMessages(user_id);
        
        if(messages.empty()) {
            return ;
        }

        LOG_INFO << "Sending " << messages.size() << " offline messages to user " << user_id;

        // 通知用户有离线消息
        p::OfflineMessageNotify notify;
        notify.set_count(messages.size());
        notify.set_timestamp(tool::getTimestamp());

        auto conn = getConnection(user_id);
        if(conn) {
            // 发送通知
            p::MessageHeader header;
            header.set_msg_type(p::MSG_OFFLINE_NOTIFY);
            header.set_timestamp(tool::getTimestamp());
            header.set_from_uid(0);
            header.set_to_uid(user_id);
        
            auto data = proto::MessageCodec::encode(header, notify);
            if(!data.empty()) {
                conn->send(data.data(), data.size());
            }
        }

        // 逐个发送离线消息
        for(const auto& msg : messages) {
            if(conn) {
                p::ChatMessage chat_msg;
                chat_msg.set_from_uid(msg.from_uid);
                chat_msg.set_to_uid(msg.to_uid);
                chat_msg.set_content(msg.content);
                chat_msg.set_msg_type(msg.msg_type);
                chat_msg.set_extra(msg.extra);
                chat_msg.set_timestamp(msg.created_at);
                chat_msg.set_msg_id(msg.msg_id);
                
                p::MessageHeader header;
                header.set_msg_type(p::MSG_CHAT);
                header.set_msg_id(msg.msg_id);
                header.set_timestamp(msg.created_at);
                header.set_from_uid(msg.from_uid);
                header.set_to_uid(msg.to_uid);

                auto data = proto::MessageCodec::encode(header, chat_msg);
                if(!data.empty()) {
                    conn->send(data.data(), data.size());
                }
            }
        }
    }

private:
    std::unordered_map<uint64_t, std::shared_ptr<TcpConnection>>* user_connections_ = nullptr;
    FriendDAO* friend_dao_ = nullptr;
    BlockDAO* block_dao_ = nullptr;

    // 响应发送
    void sendChatResponse(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, bool success, const std::string& msg) {
        p::CommonResponse response;
        response.set_code(success ? 0 : -1);
        response.set_message(msg);
        response.set_timestamp(tool::getTimestamp());

        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(header.msg_type());
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, response);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    void sendHistoryResponse(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const p::HistoryResponse& response) {
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(header.msg_type());
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, response);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    void sendUnreadCountResponse(std::shared_ptr<TcpConnection> conn,
                                const p::MessageHeader& header,
                                const p::UnreadCountResponse& response) {
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_UNREAD_COUNT);
        resp_header.set_timestamp(tool::getTimestamp());
        
        auto data = proto::MessageCodec::encode(resp_header, response);
        if (!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    void sendConversationListResponse(std::shared_ptr<TcpConnection> conn,
                                     const p::MessageHeader& header,
                                     const p::ConversationListResponse& response) {
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_CONVERSATION_LIST);
        resp_header.set_timestamp(tool::getTimestamp());
        
        auto data = proto::MessageCodec::encode(resp_header, response);
        if (!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    std::shared_ptr<TcpConnection> getConnection(uint64_t user_id) {
        if (user_connections_) {
            auto it = user_connections_->find(user_id);
            if (it != user_connections_->end()) {
                return it->second;
            }
        }
        return nullptr;
    }

    void sendSuccessResponse(std::shared_ptr<TcpConnection> conn,
                         const p::MessageHeader& header,
                         uint64_t msg_id) {
        p::CommonResponse resp;
        resp.set_code(0);
        resp.set_message("Message sent");
        resp.set_timestamp(tool::getTimestamp());

        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_COMMON_RESPONSE);
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, resp);
        if (!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }
};