#pragma once

#include "protobuf/p.h"
#include "mysql/groupDAO.h"
#include "mysql/groupmessageDAO.h"
#include "mysql/friendDAO.h"
#include "tool/logging.h"
#include "tool/tool.h"
#include "net/epoll.h"
#include <unordered_map>
#include <set>
#include <mutex>
#include "tool/deduplicator.h"
#include "tool/Check.h"

class GroupChatHandle {
public:
    GroupChatHandle() = default;

    void setUserConnections(std::unordered_map<uint64_t, std::shared_ptr<TcpConnection>>* conns) {
        user_connections_ = conns;
    }

    void setGroupDAO(GroupDAO* group_dao) {
        group_dao_ = group_dao;
    }

    void setFriendDAO(FriendDAO* friend_dao) {
        friend_dao_ = friend_dao;
    }

    // 发送群消息
    void handleGroupChat(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::GroupChatRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendGroupChatResponse(conn, header, false, "Invalid request");
            return;
        }

        uint64_t from_uid = conn->getUserID();
        if(from_uid == 0) {
            sendGroupChatResponse(conn, header, false, "User not logged in");
            return;
        }

        // 任意群聊消息都视为活跃，刷新心跳，避免高频聊天时因心跳超时被踢下线
        //OnlineManager::getInstance().updateHeartbeat(from_uid);

        uint64_t group_id = request.group_id();
        std::string content = request.content();
        int msg_type = request.msg_type();

        if(!InputValidator::validateMessageContent(content)) {
            sendGroupChatResponse(conn, header, false, "invalid message content");
            return;
        }

        // 验证是否为群组成员
        if(!group_dao_->isGroupMember(group_id, from_uid)) {
            sendGroupChatResponse(conn, header, false, "Not a member of this group");
            return;
        }

        // 检查是否被禁言
        GroupMember member;
        if(group_dao_->getMember(group_id, from_uid, member) && member.is_muted) {
            if(member.muted_until > tool::getTimestamp()) {
                sendGroupChatResponse(conn, header, false, "You are muted");
                return;
            }
        }

        // 保存消息
        GroupMessage msg;
        msg.group_id = group_id;
        msg.from_uid = from_uid;
        msg.msg_type = msg_type > 0 ? msg_type : 1;
        msg.content = content;
        msg.status = 0;
        msg.extra = request.extra();
        msg.created_at = tool::getTimestamp();

        GroupMessageDAO dao;
        uint64_t msg_id;
        if(!dao.saveGroupMessage(msg, msg_id)) {
            sendGroupChatResponse(conn, header, false, "failed to save message");
            return;
        }

        // 获取发送者用户名
        std::string sender_username;
        UserDAO user_dao;
        USER user;
        if (user_dao.getUserByID(from_uid, user)) {
            sender_username = user.nickname.empty() ? user.username : user.nickname;
        }
        if (sender_username.empty()) {
            sender_username = std::to_string(from_uid);
        }

        // 获取详细的成员列表
        auto members = group_dao_->getGroupMembers(group_id);

        MessageDeduplicator& dedup = MessageDeduplicator::getInstance();
        if(dedup.isDuplicate(msg_id)) {
            LOG_ERROR << "Duplicate group message detected, msg_id=" << msg_id << ", group_id=" << group_id << ", from=" << from_uid;
            sendSuccessResponse(conn, header, msg_id);
            return;
        }
        dedup.markProcessed(msg_id);

        {
            // 构建推送消息
            p::GroupMessagePush push_msg;
            push_msg.set_msg_id(msg_id);
            push_msg.set_msg_type(msg_type);
            push_msg.set_group_id(group_id);
            push_msg.set_from_uid(from_uid);
            push_msg.set_content(content);
            push_msg.set_from_username(sender_username);
            push_msg.set_created_at(tool::getTimestamp());

            LOG_INFO << "=== DEBUG: GroupMessagePush fields ===";
            LOG_INFO << "msg_id: " << push_msg.msg_id();
            LOG_INFO << "group_id: " << push_msg.group_id();
            LOG_INFO << "from_uid: " << push_msg.from_uid();
            LOG_INFO << "from_username: " << push_msg.from_username();
            LOG_INFO << "content: " << push_msg.content();
            LOG_INFO << "msg_type: " << push_msg.msg_type();
            LOG_INFO << "created_at: " << push_msg.created_at();

            std::string test_serialized;
            if (push_msg.SerializeToString(&test_serialized)) {
                LOG_INFO << "SerializeToString size: " << test_serialized.size();
                std::string hex;
                for (size_t i = 0; i < std::min(test_serialized.size(), size_t(50)); ++i) {
                    char buf[4];
                    snprintf(buf, sizeof(buf), "%02x ", (unsigned char)test_serialized[i]);
                    hex += buf;
                }
                LOG_INFO << "Serialized hex: " << hex;
            } else {
                LOG_ERROR << "SerializeToString failed!";
            }

            std::vector<uint64_t> offline_members;

            // 发送给所有在线成员（含发送者自己，实现回显）
            for(const auto& m : members) {

                bool is_online = false;
                if(user_connections_) {
                    auto it = user_connections_->find(m.user_id);
                    if(it != user_connections_->end()) {
                        is_online = true;

                        p::MessageHeader push_header;
                        push_header.set_msg_id(msg_id);
                        push_header.set_msg_type(p::MSG_GROUP_CHAT);
                        push_header.set_from_uid(from_uid);
                        push_header.set_to_uid(group_id);
                        push_header.set_timestamp(tool::getTimestamp());

                        LOG_INFO << "Sending GroupMessagePush to user " << m.user_id;
                        auto data = proto::MessageCodec::encode(push_header, push_msg);
                        if(!data.empty()) {
                            it->second->send(data.data(), data.size());
                            LOG_INFO << "Sent to user " << m.user_id << ", data size: " << data.size();
                        }
                    }
                }

                if(!is_online && m.user_id != from_uid) {
                    offline_members.push_back(m.user_id);
                }
            }

            // 保存离线消息（不含发送者自己）
            for(uint64_t uid : offline_members) {
                dao.saveGroupOfflineMessage(uid, group_id, msg_id);
            }

            LOG_INFO << "Group message " << msg_id << " sent to " << members.size() 
                    << " members, " << offline_members.size() << " offline";
        }

        // 最后发送响应给发送者
        //sendGroupChatResponse(conn, header, true, "Message sent");
    }

    // 获取群组历史消息
    void handleGroupHistory(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::GroupHistoryRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendGroupChatResponse(conn, header, false, "Invalid request");
            return ;
        }

        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendGroupChatResponse(conn, header, false, "User not logged in");
            return ;
        }

        uint64_t group_id = request.group_id();
        int limit = request.limit() > 0 ? request.limit() : 100;
        int64_t before_time = request.before_time();

        // 验证用户是否为群组成员
        if(!group_dao_->isGroupMember(group_id, user_id)) {
            sendGroupChatResponse(conn, header, false, "Not a member of this group");
            return ;
        }

        GroupMessageDAO dao;
        std::vector<GroupMessage> messages;

        if(before_time > 0) {
            messages = dao.getGroupHistory(group_id, limit, before_time);
        }
        else {
            messages = dao.getGroupHistory(group_id, limit);
        }

        p::GroupHistoryResponse response;
        response.set_success(true);
        response.set_group_id(group_id);

        for(const auto& msg : messages) {
            auto* item = response.add_messages();
            item->set_msg_id(msg.msg_id);
            item->set_msg_type(msg.msg_type);
            item->set_from_uid(msg.from_uid);
            item->set_from_username(msg.from_username);
            item->set_content(msg.content);
            item->set_is_recalled(msg.is_recalled);
            item->set_created_at(msg.created_at);
        }

        sendGroupHistoryResponse(conn, header, response);
        LOG_DEBUG << "Retrieved " << messages.size() << " group messages for group " << group_id;
    }

    // 撤回群组消息
    void handleRecallGroupMessage(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::RecallGroupMessageRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendGroupChatResponse(conn, header, false, "Invalid request");
            return ;
        }

        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendGroupChatResponse(conn, header, false, "Invalid request");
            return ;
        }

        uint64_t msg_id = request.msg_id() ;
        GroupMessageDAO dao;
        GroupMessage msg;
        if(!dao.getGroupMessageById(msg_id, msg)) {
            sendGroupChatResponse(conn, header, false, "Message not found");
            return ;
        }

        bool is_sender = (msg.from_uid == user_id);
        bool is_admin = group_dao_->canManageGroup(msg.group_id, user_id);

        if (!is_sender && !is_admin) {
            sendGroupChatResponse(conn, header, false, "Permission denied");
            return;
        }

        int64_t now = tool::getTimestamp();
        int64_t elapsed = now - msg.created_at;
        bool is_timeout = (elapsed > 2 * 60 * 1000);  // 超过2分钟

        // 超时2分钟不允许撤回
        if (is_sender) {
            if (is_timeout) {
                sendGroupChatResponse(conn, header, false, "Cannot recall message after 2 minutes");
                return;
            }
        }

        // 撤回
        if (!dao.recallGroupMessage(msg_id, user_id)) {
            sendGroupChatResponse(conn, header, false, "Failed to recall message");
            return;
        }

        // 通知群组成员
        p::GroupRecall recall;
        recall.set_from_uid(user_id);
        recall.set_group_id(msg.group_id);
        recall.set_msg_id(msg.msg_id);
        recall.set_recalled_at(tool::getTimestamp());

        broadcastTogroup(msg.group_id, recall, p::MSG_GROUP_RECALL);

        sendGroupChatResponse(conn, header, true, "Message recalled");
        LOG_INFO << "Group message " << msg_id << " recalled by " << user_id;
    }

    // 获取未读消息的数量
    void handleGroupUnread(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendGroupChatResponse(conn, header, false, "User not logged in");
            return ;
        }

        // 获取用户加入的群组
        GroupMessageDAO dao;
        auto groups = group_dao_->getUserGroup(user_id);

        // 响应
        p::GroupUnreadResponse response;
        response.set_success(true);
        int total_unread = 0;

        // 遍历每个数组，计算未读的数量
        for(const auto& group : groups) {
            auto read_info = dao.getGroupLastRead(user_id, group.group_id);

            auto* item = response.add_unread_items();
            item->set_group_id(group.group_id);
            item->set_group_name(group.group_name);
            item->set_unread_count(read_info.unread_count);

            total_unread += read_info.unread_count;
        }

        response.set_total_unread(total_unread);
        
        // 发送响应
        sendGroupUnreadResponse(conn, header, response);
        LOG_DEBUG << "Group unread for user " << user_id << ": " << total_unread;
    }

    // 消息标记为已读
    void handleMarkGroupRead(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::MarkGroupReadRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendGroupChatResponse(conn, header, false, "Invalid request");
            return ;
        }

        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendGroupChatResponse(conn, header, false, "User not logged in");
            return ;
        }

        uint64_t group_id = request.group_id();
        uint64_t last_msg_id = request.last_msg_id();

        // 验证是否为群组成员
        if(!group_dao_->isGroupMember(group_id, user_id)) {
            sendGroupChatResponse(conn, header, false, "Not a member of this group");
            return ;
        }

        // 最后一条消息ID
        GroupMessageDAO dao;
        if(last_msg_id == 0) {
            auto history = dao.getGroupHistory(group_id, 1);
            if(!history.empty()) {
                last_msg_id = history[0].msg_id;
            }
        }

        if(last_msg_id > 0) {
            dao.updateGroupLastRead(user_id, group_id, last_msg_id);
        }

        sendGroupChatResponse(conn, header, true, "Marked as read");
        LOG_DEBUG << "User " << user_id << " marked group " << group_id << " as read";
    }

    // 获取所有的离线消息在用户上线之后推送
    void sendAllOfflineMessage(uint64_t user_id) {
        GroupMessageDAO group_dao;
        auto group_msgs = group_dao.getGroupOfflineMessages(user_id);

        if(group_msgs.empty()) {
            return ;
        }

        LOG_INFO << "Sending " << group_msgs.size() << " group offline message to user " << user_id;

        auto conn = getConnection(user_id);
        if(!conn || conn->isClosed()) {
            LOG_ERROR << "User " << user_id << " not online, cannot send offline messages";
            return ;
        }

        // 发送群聊的离线消息
        for(auto& msg : group_msgs) {
            if (msg.from_uid == 0 && !msg.extra.empty()) {
                db::GroupMessageExtra extra;
                if (Switch::dsFromJson(msg.extra, extra)) {
                    msg.from_uid = extra.from_uid();
                    msg.content = extra.content();
                    msg.msg_type = extra.msg_type();
                    msg.group_id = extra.group_id();
                    msg.created_at = extra.created_at();
                    LOG_DEBUG << "Recovered from extra: from_uid=" << msg.from_uid 
                            << ", content=" << msg.content;
                }
            }

            // 如果恢复后还是空，跳过
            if (msg.from_uid == 0 || msg.content.empty()) {
                LOG_ERROR << "Skipping invalid offline msg: from_uid=" << msg.from_uid 
                        << ", content=" << msg.content;
                continue;
            }

            p::GroupMessagePush push_msg;
            push_msg.set_msg_id(msg.msg_id);
            push_msg.set_msg_type(msg.msg_type);
            push_msg.set_from_uid(msg.from_uid);
            push_msg.set_from_username(msg.from_username);
            push_msg.set_content(msg.content);
            push_msg.set_group_id(msg.group_id);
            push_msg.set_created_at(msg.created_at);
        
            p::MessageHeader header;
            header.set_msg_id(msg.msg_id);
            header.set_msg_type(p::MSG_GROUP_CHAT);
            header.set_timestamp(tool::getTimestamp());
            header.set_from_uid(msg.from_uid);
            header.set_to_uid(msg.group_id);


            auto data = proto::MessageCodec::encode(header, push_msg);
            if(!data.empty()) {
                conn->send(data.data(), data.size());
            }
        }

        LOG_INFO << "All group offline messages sent to user " << user_id;
    }
private:
    std::unordered_map<uint64_t, std::shared_ptr<TcpConnection>>* user_connections_ = nullptr;
    GroupDAO* group_dao_ = nullptr;
    FriendDAO* friend_dao_ = nullptr;

    std::shared_ptr<TcpConnection> getConnection(uint64_t user_id) {
        if(user_connections_) {
            auto it = user_connections_->find(user_id);
            if(it != user_connections_->end()) {
                return it->second;
            }
        }
        return nullptr;
    }

    void sendGroupChatResponse(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, bool success, const std::string& msg) {
        p::CommonResponse response;
        response.set_code(success ? 0 : -1);
        response.set_message(msg);
        response.set_timestamp(tool::getTimestamp());

        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_COMMON_RESPONSE);
        resp_header.set_timestamp(tool::getTimestamp());
        
        auto data = proto::MessageCodec::encode(resp_header, response);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    void sendGroupHistoryResponse(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const p::GroupHistoryResponse& response) {
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(header.msg_type());
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, response);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    template<typename T>
    void broadcastTogroup(uint64_t group_id, const T& message, p::MessageType type) {
        // 获取群组所有的成员列表
        auto members = group_dao_->getGroupMembers(group_id);

        // 遍历每个成员
        for(const auto& member : members) {
            // 检查连接映射是否存在
            if(user_connections_) {
                // 检查用户是否在线
                auto it = user_connections_->find(member.user_id);
                if(it != user_connections_->end()) {
                    p::MessageHeader header;
                    header.set_msg_type(type);
                    header.set_timestamp(tool::getTimestamp());

                    // 编码消息
                    auto data = proto::MessageCodec::encode(header, message);
                    if(!data.empty()) {
                        it->second->send(data.data(), data.size());
                    }
                }
            }
        }
    }


    void sendGroupUnreadResponse(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const p::GroupUnreadResponse& response) {
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_GROUP_UNREAD);
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, response);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
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