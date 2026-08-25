#pragma once

#include "protobuf/p.h"
#include "mysql/friendDAO.h"
#include "mysql/pingbiDAO.h"
#include "mysql/userDAO.h"
#include "mysql/baseDAO.h"
#include "tool/logging.h"
#include "account/Manager.h"
#include "OnlineManager.h"
#include <unordered_set>
#include <functional>


class friendHandle {
public:
    friendHandle() = default;

    // 链接管理
    void setConnection(std::function<std::shared_ptr<TcpConnection>(uint64_t)> getter) {
        get_connection_ = getter;
    }

    // 发送好友请求
    void handleAddFriend(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        LOG_INFO << "=== handleAddFriend: START ===";
        p::FriendRequest proto_req;
        if(!proto_req.ParseFromArray(body.data(), body.size())) {
            sendCommonResponse(conn, header, false, "Invalid request");
            return ;
        }

        uint64_t from_uid = header.from_uid();
        uint64_t to_uid = proto_req.to_uid();
        std::string message = proto_req.message();

        if(from_uid == to_uid) {
            sendCommonResponse(conn, header, false, "Cannot add self");
            return ;
        }

        FriendDAO dao;
        if(dao.isFriend(from_uid, to_uid)) {
            sendCommonResponse(conn, header, false, "Already friends");
            return ;
        }

        // 检查是否被屏蔽
        BlockDAO block_dao;
        if(block_dao.isBlocked(to_uid, from_uid)) {
            sendCommonResponse(conn, header, false, "You are blocked by this  user");
            return ;
        }

        LOG_INFO << "=== handleAddFriend: from=" << from_uid << ", to=" << to_uid;
        uint64_t request_id = 0;
        if(dao.sendFriendRequest(from_uid, to_uid, message, request_id)) {
            LOG_INFO << "=== handleAddFriend: SUCCESS, request_id=" << request_id;
            
            // 统一使用 MSG_COMMON_RESPONSE 通知发送方
            sendCommonResponse(conn, header, true, 
                            "Friend request sent (ID: " + std::to_string(request_id) + ")");
            
            // 通知接收方
            notifyUser(to_uid, from_uid, 
                    "You have a new friend request from " + std::to_string(from_uid), 
                    request_id);
        }
        else {
            LOG_ERROR << "=== handleAddFriend: sendFriendRequest FAILED ===";
            sendCommonResponse(conn, header, false, "Failed to send request");
        }
    }

    // 处理好友请求
    void handleProcessFriendRequest(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::ProcessFriendRequest proto_req;
        if (!proto_req.ParseFromArray(body.data(), body.size())) {
            sendCommonResponse(conn, header, false, "Invalid request");
            return;
        }
        
        uint64_t request_id = proto_req.request_id();
        bool accept = proto_req.accept();
        
        FriendDAO dao;
        if (dao.processFriendRequest(request_id, accept)) {
            FriendRequestInfo info;
            if (dao.getRequestInfo(request_id, info)) {
                if (accept) {
                    notifyUser(info.from_uid, info.to_uid, 
                            "Your friend request has been accepted",
                            request_id);
                    LOG_INFO << "Friend request accepted: " << request_id;
                } else {
                    notifyUser(info.from_uid, info.to_uid, 
                            "Your friend request has been rejected",
                            request_id);
                    LOG_INFO << "Friend request rejected: " << request_id;
                }
            }
            sendCommonResponse(conn, header, true, 
                            accept ? "Friend request accepted" : "Friend request rejected");
        } else {
            sendCommonResponse(conn, header, false, "Failed to process request");
        }
    }

    // 获取好友列表
    void handleGetFriendList(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        uint64_t user_id = header.from_uid();
        if (user_id == 0) {
            user_id = conn->getUserID();
            if (user_id == 0) {
                LOG_ERROR << "handleGetFriendList: user_id is 0";
                sendCommonResponse(conn, header, false, "User not authenticated");
                return;
            }
        }
        
        LOG_INFO << "handleGetFriendList: user_id=" << user_id;
        
        bool include_offline = true;
        FriendDAO dao;
        auto friends = dao.getFriend(user_id, include_offline);
        
        LOG_INFO << "handleGetFriendList: found " << friends.size() << " friends for user " << user_id;
        
        p::FriendListResponse response;
        for (const auto& u : friends) {
            auto* f = response.add_friends();
            f->set_user_id(u.user_id);
            f->set_username(u.username);
            f->set_nickname(u.nickname);
            f->set_avatar(u.avatar);
            f->set_status(u.status);
            f->set_is_online(OnlineManager::getInstance().isOnline(u.user_id));
            LOG_DEBUG << "  Friend: user_id=" << u.user_id << ", nickname=" << u.nickname;
        }

        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(header.msg_type());  // MSG_FRIEND_LIST
        resp_header.set_timestamp(tool::getTimestamp());
        resp_header.set_from_uid(user_id);
        resp_header.set_request_id(header.request_id());

        auto data = proto::MessageCodec::encode(resp_header, response);
        if (!data.empty()) {
            conn->send(data.data(), data.size());
            LOG_DEBUG << "handleGetFriendList: sent " << data.size() << " bytes";
        } else {
            LOG_ERROR << "handleGetFriendList: failed to encode response";
        }
    }


    // 删除好友
    void handleDeleteFriend(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::DeleteFriendRequest proto_req;
        if(!proto_req.ParseFromArray(body.data(), body.size())) {
            sendCommonResponse(conn, header, false, "Invalid request");
            return ;
        }

        uint64_t user_id = header.from_uid();
        if (user_id == 0) {
            user_id = conn->getUserID();
            if (user_id == 0) {
                sendCommonResponse(conn, header, false, "User not authenticated");
                return;
            }
        }
        
        uint64_t friend_id = proto_req.friend_id();
        if (friend_id == 0) {
            sendCommonResponse(conn, header, false, "Invalid friend ID");
            return;
        }

        FriendDAO dao;
        if(!dao.isFriend(user_id, friend_id)) {
            sendCommonResponse(conn, header, false, "Not friends");
            return ;
        }
        if(dao.deleteFriend(user_id, friend_id)) {
            sendCommonResponse(conn, header, true, "Friend deleted");
            notifyUser(friend_id, user_id, "User " + std::to_string(user_id) + " removed you from friends");
            LOG_INFO << "Friend delete: " << user_id << "<-> " << friend_id;
        }
        else {
            sendCommonResponse(conn, header, false, "Delete failed");
        }
    }

    // 屏蔽用户
    void handleBlockUser(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::BlockUserRequest proto_req;
        if(!proto_req.ParseFromArray(body.data(), body.size())) {
            sendCommonResponse(conn, header, false, "Invalid request");
            return ;
        }

        uint64_t user_id = conn->getUserID();
        uint64_t block_id = proto_req.block_id();

        BlockDAO dao;
        if(dao.blockUser(user_id, block_id)) {
            FriendDAO friend_dao;
            if(friend_dao.isFriend(user_id, block_id)) {
                if (!friend_dao.deleteFriend(user_id, block_id)) {
                    LOG_ERROR << "Failed to delete friendship when blocking: " << user_id << " -> " << block_id;
                }
            }
            sendCommonResponse(conn, header, true, "User blocked");
            LOG_INFO << "User " << user_id << "blocked" << block_id;
        }
        else {
            sendCommonResponse(conn, header, false, "Block failed");
        }
    }

    // 取消屏蔽
    void handleUnblockUser(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::UnblockUserRequest proto_req;
        if(!proto_req.ParseFromArray(body.data(), body.size())) {
            sendCommonResponse(conn, header, false, "block failed");
            return ;
        }

        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendCommonResponse(conn, header, false, "user  not logged in");
            return ;
        }

        uint64_t block_id = proto_req.block_id();
        if(block_id == 0) {
            sendCommonResponse(conn, header, false, "Invalid failed");
            return ;
        }

        BlockDAO dao;
        if(dao.unblockUser(user_id, block_id)) {
            FriendDAO friend_dao;
            if(!friend_dao.isFriend(user_id, block_id)) {
                if(friend_dao.addFriendship(user_id, block_id)) {
                    LOG_INFO << "Friendship restored between " << user_id << " and " << "block_id";
                    notifyUser(block_id, user_id, "User " + std::to_string(user_id) + "has unblocked you and restored friendship");
                }
            }
            sendCommonResponse(conn, header, true, "User unblocked");
            LOG_INFO << "User" << user_id << "unblocked" << block_id;
        }
        else {
            sendCommonResponse(conn, header, false, "Unblock failed");
        }
    }

    // 获取屏蔽列表
    void handleGetBlockList(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        uint64_t user_id = header.from_uid();
        BlockDAO dao; 
        auto block_list = dao.getBlockList(user_id);

        p::BlockListResponse response;
        for(uint64_t bid : block_list) {
            response.add_block_ids(bid);
        }
        
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(header.msg_type());
        resp_header.set_timestamp(tool::getTimestamp());
    
        auto data = proto::MessageCodec::encode(resp_header, response);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    // 验证好友关系
    bool verifyFriendship(uint64_t from_uid, uint64_t to_uid) {
        FriendDAO dao;
        return dao.isFriend(from_uid, to_uid);
    }

    // 用户上下线通知
    void broadcastOnlineStatus(uint64_t user_id, bool online) {
        FriendDAO dao;
        auto friends = dao.getFriend(user_id, true);
    
        for(const auto& f : friends) {
            notifyUser(f.user_id, "User " + std::to_string(user_id) + (online ? " came online" : " went offline"));
        }
    }

    // 查询好友在线的状态
    void handleOnlineStatus(std::shared_ptr<TcpConnection> conn,
                        const p::MessageHeader& header,
                        const std::vector<char>& body) {
        p::OnlineStatusRequest request;
        if (!request.ParseFromArray(body.data(), body.size())) {
            sendCommonResponse(conn, header, false, "Invalid request");
            return;
        }
        
        uint64_t user_id = conn->getUserID();
        if (user_id == 0) {
            sendCommonResponse(conn, header, false, "User not logged in");
            return;
        }
        
        // 获取要查询的用户列表
        std::vector<uint64_t> target_ids;
        for (int i = 0; i < request.target_ids_size(); ++i) {
            target_ids.push_back(request.target_ids(i));
        }
        
        p::OnlineStatusResponse response;
        
        // 如果没有指定目标，返回所有好友的在线状态
        if (target_ids.empty()) {
            FriendDAO dao;
            auto friends = dao.getFriend(user_id, true);
            for (const auto& friend_user : friends) {
                auto info = response.add_online_info();
                info->set_user_id(friend_user.user_id);
                info->set_username(friend_user.username);
                info->set_nickname(friend_user.nickname);
                info->set_is_online(OnlineManager::getInstance().isOnline(friend_user.user_id));
            }
        } else {
            // 查询指定用户的在线状态
            UserDAO user_dao;
            for (uint64_t target_id : target_ids) {
                auto info = response.add_online_info();
                info->set_user_id(target_id);
                info->set_is_online(OnlineManager::getInstance().isOnline(target_id));
                
                // 获取用户基本信息
                USER user;
                if (user_dao.getUserByID(target_id, user)) {
                    info->set_username(user.username);
                    info->set_nickname(user.nickname);
                    info->set_avatar(user.avatar);
                }
            }
        }
        
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_FRIEND_ONLINE_STATUS);
        resp_header.set_timestamp(tool::getTimestamp());
        
        auto data = proto::MessageCodec::encode(resp_header, response);
        if (!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    // 设置用户连接映射
    void setUserConnections(std::unordered_map<uint64_t, std::shared_ptr<TcpConnection>>* conns) {
        user_connections_ = conns;
    }

    // 设置 BlockDAO
    void setBlockDAO(BlockDAO* block_dao) {
        block_dao_ = block_dao;
    }

private:
    void sendCommonResponse(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, bool success, const std::string& msg) {
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

    void notifyUser(uint64_t user_id, uint64_t from_uid, const std::string& message, uint64_t request_id = 0) {
        LOG_INFO << "notifyUser: target=" << user_id << ", from=" << from_uid << ", msg=" << message;
        
        if (!get_connection_) {
            LOG_ERROR << "notifyUser: get_connection_ is NULL!";
            return;
        }
        
        auto conn = get_connection_(user_id);
        if (!conn) {
            LOG_WARN << "notifyUser: user " << user_id << " not online, saving offline notification";
            std::string sql = "INSERT INTO offline_notifications (user_id, from_id, type, message, request_id, created_at) "
                          "VALUES (" + std::to_string(user_id) + ", " + std::to_string(from_uid) + 
                          ", 'friend_request', '" + escapeString(message) + "', " + 
                          std::to_string(request_id) + ", " + std::to_string(tool::getTimestamp()) + ")";
            return;
        }
        
        if (conn->isClosed()) {
            LOG_WARN << "notifyUser: user " << user_id << " connection closed";
            return;
        }
        
        p::CommonResponse resp;
        resp.set_code(0);
        resp.set_message(message);
        resp.set_timestamp(tool::getTimestamp());

        p::MessageHeader header;
        header.set_msg_type(p::MSG_COMMON_REQUEST);
        header.set_timestamp(tool::getTimestamp());
        header.set_request_id(request_id);
        header.set_from_uid(from_uid);
        header.set_to_uid(user_id);

        auto data = proto::MessageCodec::encode(header, resp);
        if (!data.empty()) {
            conn->send(data.data(), data.size());
            LOG_INFO << "Notification sent to user " << user_id;
        } else {
            LOG_ERROR << "Failed to encode notification for user " << user_id;
        }
    }

    void notifyUser(uint64_t user_id, const std::string& message) {
        if (get_connection_) {
            auto conn = get_connection_(user_id);
            if (conn && !conn->isClosed()) {
                p::CommonResponse resp;
                resp.set_code(0);
                resp.set_message(message);
                resp.set_timestamp(tool::getTimestamp());

                p::MessageHeader header;
                header.set_msg_type(p::MSG_COMMON_REQUEST);
                header.set_timestamp(tool::getTimestamp());

                auto data = proto::MessageCodec::encode(header, resp);
                if (!data.empty()) {
                    conn->send(data.data(), data.size());
                }
            }
        }
    }

    std::string escapeString(const std::string& str) {
        std::string result;
        result.reserve(str.size() * 2 + 1);
        for (char c : str) {
            switch (c) {
                case '\'': result += "\\'"; break;
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                case '\0': result += "\\0"; break;
                default:   result += c; break;
            }
        }
        return result;
    }

    std::function<std::shared_ptr<TcpConnection>(uint64_t)> get_connection_;
    std::unordered_map<uint64_t, std::shared_ptr<TcpConnection>>* user_connections_ = nullptr;
    BlockDAO* block_dao_ = nullptr;
};