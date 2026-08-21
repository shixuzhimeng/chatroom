#pragma once


#include "mysql/groupDAO.h"
#include "mysql/userDAO.h"
#include "protobuf/p.h"
#include "../logging.h"
#include "../tool.h"
#include "../epoll.h"
#include <unordered_map>
#include <functional>
#include "TLS/TLS.h"
#include "../Check.h"


class GroupHandle{
public:
    void setUserConnections(std::unordered_map<uint64_t, std::shared_ptr<TcpConnection>>* conns) {
        user_connections_ = conns;
    }

    //创建群组
    void handleCreateGroup(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        LOG_INFO << "=== handleCreateGroup START ===";
        
        // 解析protobuf请求
        p::CreateGroupRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendGroupResponse(conn, header, false, "Invalid request");
            return;
        }
        LOG_INFO << "Parsed successfully, group_name=" << request.group_name();

        if(!InputValidator::validateGroupName(request.group_name())) {
            sendGroupResponse(conn, header, false, "Invalid group name");
            return;
        }

        // 验证用户登录状态
        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendGroupResponse(conn, header, false, "User not logged in");
            return;
        }
        LOG_INFO << "user_id=" << user_id;

        // 成员检查
        int initial_member_count = request.initial_member_ids_size();
        LOG_INFO << "initial_member_count=" << initial_member_count;
        if (initial_member_count < 2) {
            LOG_ERROR << "Need at least 2 other members, got " << initial_member_count;
            sendGroupResponse(conn, header, false, "Need at least 2 other members to create a group");
            return;
        }

        std::set<uint64_t> member_ids;
        member_ids.insert(user_id);     // 群主
        for (int i = 0; i < initial_member_count; ++i) {
            uint64_t uid = request.initial_member_ids(i);
            if (uid == user_id) {
                sendGroupResponse(conn, header, false, "Cannot add yourself as initial member");
                return;
            }
            if (!member_ids.insert(uid).second) {
                sendGroupResponse(conn, header, false, "Duplicate member IDs");
                return;
            }
        }

        // 验证所有用户是否存在
        UserDAO user_dao;
        std::vector<uint64_t> uids(member_ids.begin(), member_ids.end());
        auto users = user_dao.getUsersByIds(uids);
        if (users.size() != uids.size()) {
            sendGroupResponse(conn, header, false, "Some user(s) do not exist");
            return;
        }

        // 创建群组对象
        Group group;
        group.group_name = request.group_name();
        group.group_avatar = request.group_avatar();
        group.owner_id = user_id;
        group.description = request.description();
        group.announcement = request.announcement();
        group.max_members = request.max_members() > 0 ? request.max_members() : 500;
        group.is_public = request.is_public();
        group.join_type = request.join_type();
        group.created_at = tool::getTimestamp();
        group.updated_at = tool::getTimestamp();

        GroupDAO dao;
        uint64_t group_id;

        
        LOG_INFO << "Creating group without transaction...";
        
        if(!dao.createGroup(group, group_id)) {
            sendGroupResponse(conn, header, false, "Failed to create group");
            return;
        }
        LOG_INFO << "Group created: id=" << group_id;

        bool all_members_added = true;
        std::vector<uint64_t> added_users;
        
        // 添加群主
        LOG_INFO << "Adding owner to group...";
        GroupMember owner;
        owner.group_id = group_id;
        owner.user_id = user_id;
        owner.role = 2;
        owner.join_time = tool::getTimestamp();
        owner.last_read_time = tool::getTimestamp();
        owner.is_muted = false;
        
        if (!dao.addMember(owner)) {
            LOG_ERROR << "Failed to add owner " << user_id << " to group " << group_id;
            // 失败则删除群组
            dao.deleteGroup(group_id);
            sendGroupResponse(conn, header, false, "Failed to add owner");
            return;
        }
        added_users.push_back(user_id);
        LOG_INFO << "Added owner " << user_id << " to group " << group_id;

        // 添加其他成员
        for (uint64_t uid : member_ids) {
            if (uid == user_id) continue;
            GroupMember member;
            member.group_id = group_id;
            member.user_id = uid;
            member.role = 0;
            member.join_time = tool::getTimestamp();
            member.last_read_time = tool::getTimestamp();
            member.is_muted = false;
            
            if (!dao.addMember(member)) {
                LOG_ERROR << "Failed to add member " << uid << " to group " << group_id;
                all_members_added = false;
                // 记录失败的成员，继续添加其他成员
                continue;
            }
            added_users.push_back(uid);
            LOG_INFO << "Added member " << uid << " to group " << group_id;
        }

        // 如果有成员添加失败
        if (!all_members_added) {
            // 删除已添加的成员和群组
            for (uint64_t uid : added_users) {
                dao.removeMember(group_id, uid);
            }
            dao.deleteGroup(group_id);
            sendGroupResponse(conn, header, false, "Failed to add some members");
            return;
        }

        // 返回成功响应
        p::CreateGroupResponse response;
        response.set_success(true);
        response.set_group_id(group_id);
        response.set_message("Group created successfully");
        
        // 发送通知给所有成员
        p::GroupNotification notify;
        notify.set_type(p::GroupNotification::NOTIFY_NEW_MEMBER);
        notify.set_group_id(group_id);
        notify.set_user_id(user_id);
        notify.set_message("You were added to group: " + request.group_name());
        
        for (uint64_t uid : member_ids) {
            auto it = user_connections_->find(uid);
            if (it != user_connections_->end()) {
                sendNotification(it->second, notify);
            }
        }

        LOG_INFO << "Group created successfully: " << group.group_name << " (id=" << group_id << ")";
    }
    
    // 群解散
    void handleDismissGroup(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        // 解析protobuf请求
        p::DismissGroupRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendGroupResponse(conn, header, false, "Invalid request");
            return ;
        }

        // 验证用户登录状态
        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendGroupResponse(conn, header, false, "User not logged in");
            return ;
        }
        uint64_t group_id = request.group_id();
        GroupDAO dao;
        
        // 验证群主身份
        if(!dao.isGroupOwner(group_id, user_id)) {
            sendGroupResponse(conn, header, false, "Only group owner can dismiss the group");
            return ;
        }

        // 获取所有的群组成员
        auto members = dao.getGroupMembers(group_id);
    
        // 解散群聊
        if(!dao.deleteGroup(group_id)) {
            sendGroupResponse(conn, header, false, "Failed to dismiss the group");
            return ;
        }

        // 通知所有的成员
        notifyGroupDismiss(group_id, members);
        

        // 返回成功响应
        sendGroupResponse(conn, header, true, "Group dismissed");
        LOG_INFO << "Group dismissed: " << group_id << "by user " << user_id;
    }

    // 加群
    void handleJoinGroup(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        // 解析请求
        p::JoinGroupRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendGroupResponse(conn, header, false, "Invalid request");
            return ;
        }

        // 验证用户登录
        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendGroupResponse(conn, header, false, "User not logged in");
            return ;
        }

        //验证群组是否存在
        uint64_t group_id = request.group_id();
        std::string message = request.message();
        GroupDAO dao;
        Group group;
        if(!dao.getGroupByID(group_id, group)) {
            sendGroupResponse(conn, header, false, "Group not found");
            return ;
        }

        // 检查是否已经是群成员
        if(dao.isGroupMember(group_id, user_id)) {
            sendGroupResponse(conn, header, false, "Already a member of this group");
            return ;
        }

        // 检查成员数量是否满了
        if(group.member_count >= group.max_members) {
            sendGroupResponse(conn, header, false, "Group is full");
            return ;
        }

        // 根据加群方式处理
        if(group.join_type == 2) {
            sendGroupResponse(conn, header, false, "This group does not allow new members");
            return ;
        }

        if(group.join_type == 0) {
            TransactionGuard tx(dao);

            GroupMember member;
            member.group_id = group_id;
            member.user_id = user_id;
            member.role = 0;
            member.join_time = tool::getTimestamp();
            member.last_read_time = tool::getTimestamp();

            if(!dao.addMember(member)) {
                LOG_ERROR << "Failed to add member to group" << group_id << "for user " << user_id;
                tx.rollback();
                sendGroupResponse(conn, header, false, "Failed to join group");
                return ;
            }

            tx.commit();

            // 通知成员
            notifyNewMember(group_id, user_id);

            sendGroupResponse(conn, header, true, "Joined group successfully");

            LOG_INFO << "User " << user_id << "joined group " << group_id;
        }
        else {
            GroupJoinRequest req;
            req.group_id = group_id;
            req.from_uid = user_id;
            req.to_uid = group.owner_id;
            req.message = message;
            req.status = 0;
            req.created_at = tool::getTimestamp();
            req.updated_at = tool::getTimestamp();

            if(!dao.addJoinRequest(req)) {
                LOG_ERROR << "Failed to add join request for group " << group_id << "by user " << user_id;
                sendGroupResponse(conn, header, false, "Failed to submit join request");
                return ;
            }

            // 通知群主和管理员
            notifyJoinrequest(group_id, user_id, message);

            sendGroupResponse(conn, header, true, "submit join submit");
            LOG_INFO << "User " << user_id << "requested to join group " << group_id;
        }
    }

    // 处理申请加入群组
    void handleProcessJoinRequest(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::ProcessJoinRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendGroupResponse(conn, header, false, "Invalid request");
            return ;
        }

        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendGroupResponse(conn, header, false, "User not logged in");
            return ;
        }
        
        uint64_t request_id = request.request_id();
        bool accept = request.accept();

        GroupDAO dao;
        GroupJoinRequest req;
        // 获取申请详情
        if(!dao.getJoinRequest(request_id, req)) {
            sendGroupResponse(conn, header, false, "Request not found");
            return ;
        }

        // 验证权限群主和管理员
        if(!dao.isGroupOwner(req.group_id, user_id) && !dao.isGroupAdmin(req.group_id, user_id)) {
            sendGroupResponse(conn, header, false, "Permission denied");
            return ;
        }

        if(!dao.processJoinRequest(request_id, accept, user_id)) {
            sendGroupResponse(conn, header, false, "Failed to process request");
            return ;
        }

        // 通知申请人
        notifyRequestProcessed(req.from_uid, req.group_id, accept);
        
        std::string msg = accept ? "Request accepted" : "request rejuected";
        sendGroupResponse(conn, header, true, msg);
        LOG_INFO << "Join request " << request_id << " processed by " << user_id;
    }

    // 处理用户退出群组
    void handleLeaveGroup(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        // 解析请求
        p::LeaveGroupRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendGroupResponse(conn, header, false, "Invalid request");
            return ;
        }

        // 验证用户登录
        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendGroupResponse(conn, header, false, "User not logged in");
            return ;
        }
        
        uint64_t group_id = request.group_id();
        GroupDAO dao;
        // 检查是否为成员
        if(!dao.isGroupMember(group_id, user_id)) {
            sendGroupResponse(conn, header, false, "Not a member of this group");
            return ;
        }
        // 检查是否为群主
        if(dao.isGroupOwner(group_id, user_id)) {
            sendGroupResponse(conn, header, false, "Group owner cannot leave, please dismiss the group");
            return ;
        }

        // 退出群组
        if(!dao.removeMember(group_id, user_id)) {
            sendGroupResponse(conn, header, false, "Failed to leave group");
            return ;
        }

        // 通知群成员
        notifyMemberLeft(group_id, user_id);
        
        sendGroupResponse(conn, header, true, "Left group successfully");
        LOG_INFO << "User " << user_id << "left group" << group_id;
    }
    
    // 设置或是取消管理员
    void handleSetAdmin(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        // 解析请求
        p::SetAdminRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendGroupResponse(conn, header, false, "Invalid request");
            return ;
        }

        // 验证用户登录
        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendGroupResponse(conn, header, false, "User not logged in");
            return ;
        }

        uint64_t group_id = request.group_id();
        uint64_t target_uid = request.target_uid();
        bool is_admin = request.is_admin();

        GroupDAO dao;
        TransactionGuard tx(dao);

        // 处理权限验证
        if(!dao.isGroupOwner(group_id, user_id)) {
            tx.rollback();
            sendGroupResponse(conn, header, false, "Only owner can set admin");
            return ;
        }

        // 检验（自己不能设置自己）
        if(user_id == target_uid) {
            tx.rollback();
            sendGroupResponse(conn, header, false, "Cannot set yourself");
            return ;
        }

        // 验证目标用户是否为成员
        if(!dao.isGroupMember(group_id, target_uid)) {
            tx.rollback();
            sendGroupResponse(conn, header, false, "User is not a member in this group");
            return ;
        }

        // 不能处理群主的管理员身份
        if(dao.isGroupOwner(group_id, target_uid)) {
            tx.rollback();
            sendGroupResponse(conn, header, false, "Cannot change owner's role");
            return ;
        }
        
        // 获取目标用户当前的身份
        GroupMember target_member;
        if(!dao.getMember(group_id, target_uid, target_member)) {
            tx.rollback();
            sendGroupResponse(conn, header, false, "Failed to get target_member");
            return ;
        }

        // 如果已经是目标角色则不需要更新
        if((is_admin && target_member.role == 1) || (!is_admin && target_member.role == 0)) {
            tx.rollback();
            sendGroupResponse(conn, header, false, is_admin ? "User is already adnim" : "User is already not admin");
            return ;
        }

        // 更新角色
        int role = is_admin ? 1 : 0;
        if(!dao.updateMemberRole(group_id, target_uid, role)) {
            tx.rollback();
            sendGroupResponse(conn, header, false, "Failed to update admin");
            return ;
        }

        tx.commit();

        // 通知目标用户
        notifyRoleChanged(group_id, target_uid, role);
    
        // 返回响应
        std::string msg = is_admin ? "User is now admin" : "User is no longer admin";
        sendGroupResponse(conn, header, true, msg);
        LOG_INFO << "User " << target_uid << "admin status set to" << is_admin << "by " << user_id << " in group " << group_id;
    }

    // 踢人
    void Kickmember(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        // 解析请求
        p::KickMemberRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendGroupResponse(conn, header, false, "Invalid request");
            return ;
        }

        // 验证用户登录
        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendGroupResponse(conn, header, false, "User not logged in");
            return ;
        }

        uint64_t group_id = request.group_id();
        uint64_t target_id = request.target_uid();

        GroupDAO dao;

        // 验证权限
        if(!dao.canManageGroup(group_id, user_id)) {
            sendGroupResponse(conn, header, false, "Permission denied");
            return ;
        }

        // 不能踢掉群主
        if(dao.isGroupOwner(group_id, target_id)) {
            sendGroupResponse(conn, header, false, "Cannot kick owner");
            return ;
        }

        // 除了群主，管理员不能踢掉管理员
        if(dao.isGroupAdmin(group_id, target_id) && !dao.isGroupOwner(group_id, user_id)) {
            sendGroupResponse(conn, header, false, "Cannot kick member admin");
            return ;
        }

        // 踢人
        if (!dao.removeMember(group_id, target_id)) {
            sendGroupResponse(conn, header, false, "Failed to kick member");
            return;
        }

        // 通知被踢掉的用户和群组成员
        notifyMemberLeft(group_id, target_id);
        notifyKicked(group_id, target_id);

        sendGroupResponse(conn, header, true, "Member kicked success");
        LOG_INFO << "User " << target_id << "Kicked from group " << group_id;
    }

    // 获取群组列表
    void handleGetGroupList(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendGroupResponse(conn, header, false, "User not logged in");
            return ;
        }

        GroupDAO dao;
        auto groups = dao.getUserGroup(user_id);
    
        p::GroupListResponse response;
        response.set_success(true);

        for(const auto& g : groups) {
            auto* group_info = response.add_groups();
            group_info->set_group_id(g.group_id);
            group_info->set_group_name(g.group_name);
            group_info->set_group_avatar(g.group_avatar );
            group_info->set_owner_id(g.owner_id);
            group_info->set_owner_name(g.owner_name);
            group_info->set_description(g.description);
            group_info->set_member_count(g.member_count);
            group_info->set_is_public(g.is_public);
        }

        sendGroupListResponse(conn, header, response);
        LOG_INFO << "User " << user_id << "retrieved " << groups.size() << " groups";
    }

    // 获取群组成员列表
    void handleGetGroupMembers(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::GetGroupMembersRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendGroupResponse(conn, header, false, "Invalid Failed");
            return ;
        }

        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendGroupResponse(conn, header, false, "User not logged in");
            return ;
        }

        uint64_t group_id = request.group_id();

        // 验证用户是否为成员
        GroupDAO dao;
        if(!dao.isGroupMember(group_id, user_id)) {
            sendGroupResponse(conn, header, false, "Not a member of this group");
            return ;
        }

        auto members = dao.getGroupMembers(group_id);

        p::GroupMembersResponse response;
        response.set_success(true);
        response.set_group_id(group_id);
    
        for(const auto& m : members) {
            auto* member_info = response.add_members();
            member_info->set_user_id(m.user_id);
            member_info->set_username(m.username);
            member_info->set_nickname(m.nickname);
            member_info->set_role(m.role);
            member_info->set_is_muted(m.is_muted);
            member_info->set_join_time(m.join_time);
        }
    
        sendGroupMembersResponse(conn, header, response);

        LOG_INFO << "Retrieved " << members.size() << " members for groups " << group_id;
    }

    // 获取待处理的申请
    void handleGetPendingRequests(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::GetPendingRequestsRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendGroupResponse(conn, header, false, "Invalid request");
            return ;
        }

        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendGroupResponse(conn, header, false, "User not logged in");
            return ;
        }

        // 验证权限
        uint64_t group_id = request.group_id();
        GroupDAO dao;
        if(!dao.canManageGroup(group_id, user_id)) {
            sendGroupResponse(conn, header, false, "Permissed demied");
            return ;
        }

        auto requests = dao.getRequests(group_id);

        p::PendingRequestsResponse response;
        response.set_success(true);

        for(const auto& req : requests) {
            auto* req_info = response.add_requests();
            req_info->set_request_id(req.request_id);
            req_info->set_from_uid(req.from_uid);
            req_info->set_from_username(req.from_username);
            req_info->set_message(req.message);
            req_info->set_created_at(req.created_at);
        }

        sendPendingRequestsResponse(conn, header, response);
        LOG_DEBUG << "Retrieved " << requests.size() << " pending requests for group " << group_id;
    }
private:
    std::unordered_map<uint64_t, std::shared_ptr<TcpConnection>>* user_connections_ = nullptr;

    // 发送群组响应
    void sendGroupResponse(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, bool success, const std::string& msg) {
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
    
    // 重载发送群组响应
    void sendGroupResponse(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const google::protobuf::Message& response, p::MessageType type) {
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(type);
        resp_header.set_timestamp(tool::getTimestamp());
    
        auto data = proto::MessageCodec::encode(resp_header, response);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    // 通知群组解散
    void notifyGroupDismiss(uint64_t group_id, const std::vector<GroupMember>& members) {
        p::GroupNotification notify;
        notify.set_type(p::GroupNotification::NOTIFY_GROUP_DISMISSED);
        notify.set_group_id(group_id);
        notify.set_message("The group has been dismissed");
    
        // 通知所有的成员
        for(const auto& member : members) {
            if(user_connections_) {
                auto it = user_connections_->find(member.user_id);
                if(it != user_connections_->end()) {
                    sendNotification(it->second, notify);
                }
            }
        }
    
    }

    void notifyNewMember(uint64_t group_id, uint64_t user_id) {
        p::GroupNotification notify;
        notify.set_type(p::GroupNotification::NOTIFY_NEW_MEMBER);
        notify.set_group_id(group_id);
        notify.set_user_id(user_id);
        notify.set_message("New member joined the group");

        broadcastTogroup(group_id, notify);
    }

    // 发送通知
    void sendNotification(std::shared_ptr<TcpConnection> conn, const p::GroupNotification& notify) {
        p::MessageHeader header;
        header.set_msg_type(p::MSG_GROUP_NOTIFICATION);
        header.set_timestamp(tool::getTimestamp());
    
        auto data = proto::MessageCodec::encode(header, notify);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    // 通知对方申请已经被处理
    void notifyRequestProcessed(uint64_t user_id, uint64_t group_id, bool accept) {
        if(user_connections_) {
            auto it = user_connections_->find(user_id);
            if(it != user_connections_->end()) {
                p::GroupNotification notify;
                notify.set_type(accept ? p::GroupNotification::NOTIFY_REQUEST_ACCEPTED : p::GroupNotification::NOTIFY_REQUEST_REJECTED);
                notify.set_group_id(group_id);
                notify.set_message(accept ? "Your join request was acceptd" : "Your join request was rejected");
            
                sendNotification(it->second, notify);
            }
        }
    }

    // 向群组所有成员通知
    void broadcastTogroup(uint64_t group_id, const p::GroupNotification& notify) {
        GroupDAO dao;
        auto members = dao.getGroupMembers(group_id);
    
        for(const auto& member : members) {
            if(user_connections_) {
                auto it = user_connections_->find(member.user_id);
                if(it != user_connections_->end()) {
                    sendNotification(it->second, notify);
                }
            }
        }
    }

    // 通知成员退出
    void notifyMemberLeft(uint64_t group_id, uint64_t user_id) {
        p::GroupNotification notify;
        notify.set_type(p::GroupNotification::NOTIFY_MEMBER_LEFT);
        notify.set_user_id(user_id);
        notify.set_group_id(group_id);
        notify.set_message("Member left the group");
    
        broadcastTogroup(group_id, notify);
    }

    //
    void notifyJoinrequest(uint64_t group_id, uint64_t from_id, const std::string& message) {
        // 通知群主和管理员
        GroupDAO dao;
        auto members = dao.getGroupMembers(group_id);

        p::GroupNotification notify;
        notify.set_type(p::GroupNotification::NOTIFY_JOIN_REQUEST);
        notify.set_group_id(group_id);
        notify.set_user_id(from_id);
        notify.set_message(message);

        for(const auto& member : members) {
            if(member.role >= 1 && user_connections_) {
                auto it = user_connections_->find(member.user_id);
                if(it != user_connections_->end()) {
                    sendNotification(it->second, notify);
                }
            }
        }
    }

    // 通知身份被更改
    void notifyRoleChanged(uint64_t group_id, uint64_t user_id, int role) {
        if(user_connections_) {
            auto it = user_connections_->find(user_id);
            if(it != user_connections_->end()) {
                p::GroupNotification notify;
                notify.set_type(p::GroupNotification::NOTIFY_ROLE_CHANGED);
                notify.set_group_id(group_id);
                notify.set_user_id(user_id);
                notify.set_message(role == 1 ? "You are now an admin" : "You are no longer an admin");
                sendNotification(it->second, notify);
            }
        }
    }

    // 通知成员用户被踢掉
    void notifyKicked(uint64_t user_id, uint64_t group_id) {
        if (user_connections_) {
            auto it = user_connections_->find(user_id);
            if (it != user_connections_->end()) {
                p::GroupNotification notify;
                notify.set_type(p::GroupNotification::NOTIFY_KICKED);
                notify.set_group_id(group_id);
                notify.set_message("You were kicked from the group");
                sendNotification(it->second, notify);
            }
        }
    }

    void sendGroupListResponse(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const p::GroupListResponse& response) {
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(header.msg_type());
        resp_header.set_timestamp(tool::getTimestamp());
    
        auto data = proto::MessageCodec::encode(resp_header, response);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    void sendGroupMembersResponse(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const p::GroupMembersResponse& response) {
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_GROUP_MEMBERS);
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, response);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    void sendPendingRequestsResponse(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const p::PendingRequestsResponse& response) {
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(header.msg_type());
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, response);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }
};