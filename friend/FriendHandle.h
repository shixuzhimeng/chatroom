#pragma once

#include "protobuf/p.h"
#include "mysql/friendDAO.h"
#include "mysql/pingbiDAO.h"
#include "mysql/userDAO.h"
#include "../logging.h"
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

        if(dao.sendFriendRequest(from_uid, to_uid, message)) {
            sendCommonResponse(conn, header, true, "Friend requset sent");
            notifyUser(to_uid, "You have a new friend requset from " + std::to_string(from_uid));
        
            LOG_INFO << "Friend requset from " << from_uid << " to " << to_uid;
        }
        else {
            sendCommonResponse(conn, header, false, "Failed to send requset");
        }
    }

    // 处理好友请求
     void handleProcessFriendRequest(std::shared_ptr<TcpConnection> conn,
                                    const p::MessageHeader& header,
                                    const std::vector<char>& body) {
        p::ProcessFriendRequest proto_req;
        if (!proto_req.ParseFromArray(body.data(), body.size())) {
            sendCommonResponse(conn, header, false, "Invalid request");
            return;
        }
        
        uint64_t request_id = proto_req.request_id();
        bool accept = proto_req.accept();
        
        FriendDAO dao;
        if (dao.processFriendRequest(request_id, accept)) {
            if (accept) {
                sendCommonResponse(conn, header, true, "Friend request accepted");
                // 通知对方
                FriendRequestInfo info;
                if (dao.getRequestInfo(request_id, info)) {
                    notifyUser(info.from_uid, "Your friend request has been accepted");
                }
                LOG_INFO << "Friend request accepted: " << request_id;
            } else {
                sendCommonResponse(conn, header, true, "Friend request rejected");
            }
        } else {
            sendCommonResponse(conn, header, false, "Failed to process request");
        }
    }

    void handleGetFriendList(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        uint64_t user_id = header.from_uid();
        bool include_offline = true;

        FriendDAO dao;
        auto friends = dao.getFriend(user_id, include_offline);
        p::FriendListResponse response;
        for(const auto& u : friends) {
            auto* f = response.add_friends();
            f->set_user_id(u.user_id);
            f->set_username(u.username);
            f->set_nickname(u.nickname);
            f->set_avatar(u.avatar);
            f->set_status(u.status);
            f->set_is_online(OnlineManager::getInstance().isOnline(u.user_id));
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


    // 删除好友
    void handleDeleteFriend(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::DeleteFriendRequest proto_rep;
        if(!proto_rep.ParseFromArray(body.data(), body.size())) {
            sendCommonResponse(conn, header, false, "Invalid requset");
            return ;
        }

        uint64_t user_id = header.from_uid();
        uint64_t friend_id = proto_rep.friend_id();

        FriendDAO dao;
        if(!dao.isFriend(user_id, friend_id)) {
            sendCommonResponse(conn, header, false, "Not friends");
            return ;
        }
        if(dao.deleteFriend(user_id, friend_id)) {
            sendCommonResponse(conn, header, true, "Friend delete" );
            notifyUser(friend_id, "User " + std::to_string(user_id) + "removed you from friends");
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

        uint64_t user_id = header.from_uid();
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

        uint64_t user_id = header.from_uid();
        uint64_t block_id = proto_req.block_id();

        BlockDAO dao;
        if(dao.unblockUser(user_id, block_id)) {
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
        auto block_id = dao.getBlockList(user_id);

        p::BlockListResponse response;
        for(uint64_t bid : block_id) {
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

    private:
    void sendCommonResponse(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, bool success, const std::string& msg) {
        p::CommonResponse response;
        response.set_code(success ? 0 : 1);
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

    void notifyUser(uint64_t user_id, const std::string& message) {
        if(get_connection_) {
            auto conn = get_connection_(user_id);
            if(conn && !conn->isClosed()) {
                conn->send("System: " + message);
            }
        }
    }

    std::function<std::shared_ptr<TcpConnection>(uint64_t)> get_connection_;

};