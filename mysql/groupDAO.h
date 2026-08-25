#pragma once

#include "baseDAO.h"
#include "tool/logging.h"
#include "tool/tool.h"
#include <string>
#include <map>
#include <set>
#include <vector>
#include "TranscationGuard.h"

struct Group{
    uint64_t group_id = 0;
    std::string group_name;
    std::string group_avatar;
    uint64_t owner_id = 0;
    std::string owner_name;
    std::string description;
    std::string announcement;
    int max_members = 500;
    int member_count = 0;
    bool is_public = true;
    int join_type = 0;  // 0:自由加入, 1:验证加入, 2:禁止加入
    int64_t created_at = 0;
    int64_t updated_at = 0;
};


struct GroupMember{
    uint64_t member_id = 0;
    uint64_t group_id = 0;
    uint64_t user_id = 0;
    std::string username;
    std::string nickname;
    int role = 0;  // 0:普通成员, 1:管理员, 2:群主
    int64_t join_time = 0;
    int64_t last_read_time = 0;
    bool is_muted = false;
    int64_t muted_until = 0;
};

struct GroupJoinRequest {
    uint64_t request_id = 0;
    uint64_t group_id = 0;
    uint64_t from_uid = 0;
    uint64_t to_uid = 0;
    std::string from_username;
    std::string message;
    int status = 0;  // 0:待处理, 1:已接受, 2:已拒绝, 3:已忽略
    int64_t created_at = 0;
    int64_t updated_at = 0;
};


class GroupDAO : public BaseDAO {
public:
    // 建群
    bool createGroup(const Group& group, uint64_t& group_id) {
        LOG_INFO << "Start creategroup";
        LOG_INFO << "Group name: " << group.group_name;
        LOG_INFO << "Owner ID: " << group.owner_id;

        std::string sql = "INSERT INTO `groups` (group_name, group_avatar, owner_id, description, "
                        "announcement, max_members, is_public, join_type, created_at, updated_at) VALUES ('";
        sql += escapeString(group.group_name) + "', '";
        sql += escapeString(group.group_avatar) + "', ";
        sql += std::to_string(group.owner_id) + ", '";
        sql += escapeString(group.description) + "', '";
        sql += escapeString(group.announcement) + "', ";
        sql += std::to_string(group.max_members) + ", ";
        sql += std::to_string(group.is_public ? 1 : 0) + ", ";
        sql += std::to_string(group.join_type) + ", ";
        sql += std::to_string(group.created_at) + ", ";
        sql += std::to_string(group.updated_at) + ")";

        LOG_INFO << "Create group SQL: " << sql;

        if(!executeUpdate(sql)) {
            LOG_ERROR << "Create group failed";
            return false;
        }

        group_id = getLastInsertID();
        LOG_INFO << "Group inserted, last_insert_id: " << group_id;

        // 不在这里添加群主，由调用者统一添加
        LOG_INFO << "Group record created: " << group.group_name << " (id=" << group_id << ")";
        return true;
    }

    // 由群组ID获取群
    bool getGroupByID(uint64_t group_id, Group& group)  {
        std::string sql = "SELECT g.*, u.username as owner_name FROM `groups` g "
                          "LEFT JOIN users u ON g.owner_id = u.user_id "
                          "WHERE g.group_id = " + std::to_string(group_id);

        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result) || result.empty()) {
            return false;
        }
        fillGroupFromMap(result[0], group);
    

        // 获取成员数量
        std::string count_sql = "SELECT COUNT(*) as count FROM group_members WHERE group_id = " + std::to_string(group_id);
        
        std::vector<std::map<std::string, std::string>> count_result;
        if(executeQuery(count_sql, count_result) && !count_result.empty()) {
            group.member_count = std::stoi(count_result[0]["count"]);
        }

        return true;
    }

    // 添加成员
    bool addMember(const GroupMember& member) {
        std::string sql = "INSERT IGNORE INTO group_members (group_id, user_id, role, nickname, "
        "join_time, last_read_time, is_muted, muted_until) VALUES (";

        sql += std::to_string(member.group_id) + ", ";
        sql += std::to_string(member.user_id) + ", ";
        sql += std::to_string(member.role) + ", '";
        sql += escapeString(member.nickname) + "', ";
        sql += std::to_string(member.join_time) + ", ";
        sql += std::to_string(member.last_read_time) + ", ";
        sql += std::to_string(member.is_muted ? 1 : 0) + ", ";
        sql += std::to_string(member.muted_until) + ")";

        LOG_DEBUG << "Add member SQL: " << sql;

        bool result = executeUpdate(sql);

        LOG_INFO << "addMember executeUpdate result: " << (result ? "true" : "false");
        return result;
    }

    bool getGroupByName(const std::string& name, std::vector<Group>& groups) {
        std::string sql = "SELECT g.*, u.username as owner_name FROM `groups` g "
                          "LEFT JOIN users u ON g.owner_id = u.user_id "
                          "WHERE g.group_name LIKE '%" + escapeString(name) + "%' "
                          "ORDER BY g.group_id DESC LIMIT 100";
        
        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result)) {
            return false;
        }

        for(const auto& row : result) {
            Group group;
            fillGroupFromMap(row, group);
            groups.push_back(group);
        }

        return true;
    }

    bool updateGroupInfo(const Group& group) {
        std::string sql = "UPDATE `groups` SET "
                          "group_name = '" + escapeString(group.group_name) + "', "
                          "group_avatar = '" + escapeString(group.group_avatar) + "', "
                          "description = '" + escapeString(group.description) + "', "
                          "announcement = '" + escapeString(group.announcement) + "', "
                          "max_members = " + std::to_string(group.max_members) + ", "
                          "is_public = " + std::to_string(group.is_public ? 1 : 0) + ", "
                          "join_type = " + std::to_string(group.join_type) + ", "
                          "updated_at = " + std::to_string(tool::getTimestamp()) + " "
                          "WHERE group_id = " + std::to_string(group.group_id);

        return executeUpdate(sql);
    }

    // 删除群聊
    bool deleteGroup(uint64_t group_id) {
        // 先删除所有成员
        std::string del_member = "DELETE FROM group_members WHERE group_id = " + std::to_string(group_id);
        if(!executeUpdate(del_member)) {
            LOG_ERROR << "Failed to delete group members for group " << group_id;
            // 继续执行，尝试删除群组
        }
        
        // 再删除群组
        std::string del_group = "DELETE FROM `groups` WHERE group_id = " + std::to_string(group_id);
        if(!executeUpdate(del_group)) {
            LOG_ERROR << "Failed to delete group " << group_id;
            return false;
        }
        
        LOG_INFO << "Group deleted: " << group_id;
        return true;
    }

    // 删除成员
    bool removeMember(uint64_t group_id, uint64_t user_id) {
        std::string sql = "DELETE FROM group_members WHERE group_id = " + std::to_string(group_id) + 
                          " AND user_id = " + std::to_string(user_id);

        return executeUpdate(sql);
    }

    // 更新群组成员置位
    bool updateMemberRole(uint64_t group_id, uint64_t user_id, int role) {
        std::string sql = "UPDATE group_members SET role = " + std::to_string(role) + " "
                          "WHERE group_id = " + std::to_string(group_id) + 
                          " AND user_id = " + std::to_string(user_id);
        return executeUpdate(sql);
    }

    // 更改成员昵称
    bool updateMemberNickname(uint64_t group_id, uint64_t user_id, const std::string& nickname) {
        std::string sql = "UPDATE group_members SET nickname = '" + escapeString(nickname) + "' "
                          "WHERE group_id = " + std::to_string(group_id) + 
                          " AND user_id = " + std::to_string(user_id);

        return executeUpdate(sql);
    }

    bool deleteAllgroup(uint64_t user_id) {
        char sql[512];
        snprintf(sql, sizeof(sql), "DELETE FROM group_members WHERE user_id = %lu", user_id);

        return executeUpdate(sql);
    }

    // 更新用户最后在群组的时间
    bool updateLastReadTime(uint64_t group_id, uint64_t user_id) {
        std::string sql = "UPDATE group_members SET last_read_time = " + std::to_string(tool::getTimestamp()) + " "
                          "WHERE group_id = " + std::to_string(group_id) + 
                          " AND user_id = " + std::to_string(user_id);
        return executeUpdate(sql);
    }

    // 设置禁言
    bool setMuteStatus(uint64_t group_id, uint64_t user_id, bool muted, int64_t until = 0) {
        std::string sql = "UPDATE group_members SET is_muted = " + std::to_string(muted ? 1 : 0) + 
                          ", muted_until = " + std::to_string(until) + " "
                          "WHERE group_id = " + std::to_string(group_id) + 
                          " AND user_id = " + std::to_string(user_id);

        return executeUpdate(sql);
    }

    // 获取群组成员
    bool getMember(uint64_t group_id, uint64_t user_id, GroupMember& member) {
        std::string sql = "SELECT gm.*, u.username FROM group_members gm "
                          "LEFT JOIN users u ON gm.user_id = u.user_id "
                          "WHERE gm.group_id = " + std::to_string(group_id) + 
                          " AND gm.user_id = " + std::to_string(user_id);

        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result) || result.empty()) {
            return false;
        }

        fillMemberFromMap(result[0], member);

        return true;
    }

    // 判断是否为群组成员
    bool isGroupMember(uint64_t group_id, uint64_t user_id) {
        GroupMember member;
        return getMember(group_id, user_id, member);
    }

    // 查询成员列表
    std::vector<GroupMember> getGroupMembers(uint64_t group_id) {
        std::vector<GroupMember> members;
        std::string sql = "SELECT gm.*, u.username FROM group_members gm "
                          "LEFT JOIN users u ON gm.user_id = u.user_id "
                          "WHERE gm.group_id = " + std::to_string(group_id) + " "
                          "ORDER BY gm.role DESC, gm.join_time ASC";
                        
        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result)) {
            return members;
        }

        for(const auto& row : result) {
            GroupMember member;
            fillMemberFromMap(row, member);
            members.push_back(member);
        }

        return members;
    }

    // 获取用户加入的群组
    std::vector<Group> getUserGroup(uint64_t user_id) {
        std::vector<Group> groups;
        std::string sql = "SELECT g.*, u.username as owner_name FROM `groups` g "
                          "LEFT JOIN users u ON g.owner_id = u.user_id "
                          "WHERE g.group_id IN (SELECT group_id FROM group_members WHERE user_id = " + std::to_string(user_id) +
                          ") " "ORDER BY g.updated_at DESC";
        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result)) {
            return groups;
        }

        for(const auto& row : result) {
            Group group;
            fillGroupFromMap(row, group);
            groups.push_back(group);
        }

        return groups;
    }

    // 是否为群主
    bool isGroupOwner(uint64_t group_id, uint64_t user_id) {
        GroupMember member;
        if(!getMember(group_id, user_id, member)) {
            return false;
        }

        return member.role == 2;
    }


    // 是否为管理员
    bool isGroupAdmin(uint64_t group_id, uint64_t user_id) {
        GroupMember member;
        if(!getMember(group_id, user_id, member)) {
            return false;
        }
        return member.role >= 1;
    }

    // 是否有权限管理群组
    bool canManageGroup(uint64_t group_id, uint64_t user_id) {
        return isGroupOwner(group_id, user_id) || isGroupAdmin(group_id, user_id);
    }

    // 入群申请（返回生成的 request_id）
    bool addJoinRequest(const GroupJoinRequest& request, uint64_t& request_id) {
        std::string sql = "INSERT INTO group_invites (group_id, from_uid, to_uid, message, "
                          "status, created_at, updated_at) VALUES (";
        sql += std::to_string(request.group_id) + ", ";
        sql += std::to_string(request.from_uid) + ", ";
        sql += std::to_string(request.to_uid) + ", '";
        sql += escapeString(request.message) + "', ";
        sql += std::to_string(request.status) + ", ";
        sql += std::to_string(tool::getTimestamp()) + ", "; // created_at
        sql += std::to_string(tool::getTimestamp()) + ")";  // updated_at

        if(!executeUpdate(sql)) {
            return false;
        }
        request_id = getLastInsertID();
        return true;
    }

    // 详细查看入群申请
    bool getJoinRequest(uint64_t request_id, GroupJoinRequest& request) {
        std::string sql = "SELECT gi.*, u.username as from_username FROM group_invites gi "
                          "LEFT JOIN users u ON gi.from_uid = u.user_id "
                          "WHERE gi.request_id = " + std::to_string(request_id);
        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result) || result.empty()) {
            return false;
        }

        fillRequestFromMap(result[0], request);

        return true;
    }

    // 获取所有的入群申请
    std::vector<GroupJoinRequest> getRequests(uint64_t group_id) {
        std::vector<GroupJoinRequest> requests;
        std::string sql = "SELECT gi.*, u.username as from_username FROM group_invites gi "
                          "LEFT JOIN users u ON gi.from_uid = u.user_id "
                          "WHERE gi.group_id = " + std::to_string(group_id) + 
                          " AND gi.status = 0 "
                          "ORDER BY gi.created_at DESC";

        std::vector<std::map<std::string, std::string>> result;

        if(!executeQuery(sql, result)) {
            return requests;
        }

        for(const auto& row : result) {
            GroupJoinRequest req;
            fillRequestFromMap(row, req);
            requests.push_back(req);
        }

        return requests;
    }


    // 获取用户的所有的入群申请
    std::vector<GroupJoinRequest> getUserJoinRequest(uint64_t user_id) {
        std::vector<GroupJoinRequest> requests;
        std::string sql = "SELECT gi.*, u.username as from_username FROM group_invites gi "
                          "LEFT JOIN users u ON gi.from_uid = u.user_id "
                          "WHERE gi.from_uid = " + std::to_string(user_id) + 
                          " ORDER BY gi.created_at DESC";

        std::vector<std::map<std::string, std::string>> result;

        if(!executeQuery(sql, result)) {
            return requests;
        }

        for(const auto& row : result) {
            GroupJoinRequest req;
            fillRequestFromMap(row, req);
            requests.push_back(req);
        }
        
        return requests;
    }

    // 入群申请处理
    bool processJoinRequest(uint64_t request_id, bool accept, uint64_t admin_id = 0) {
        TransactionGuard tx(*this);
        int status = accept ? 1 : 2;
        std::string sql = "UPDATE group_invites SET status = " + std::to_string(status) + ", "
                          "updated_at = " + std::to_string(tool::getTimestamp()) + " "
                          "WHERE request_id = " + std::to_string(request_id);

        if(!executeUpdate(sql)) {
            return false;
        }

        if(accept) {
            // 获取请求申请
            GroupJoinRequest req;
            if(!getJoinRequest(request_id, req)) {
                return false;
            }

            // 将用户加入群组
            GroupMember member;
            member.group_id = req.group_id;
            member.user_id = req.from_uid;
            member.role = 0;
            member.join_time = tool::getTimestamp();
            member.last_read_time = tool::getTimestamp();

            if(!addMember(member)) {
                LOG_ERROR << "Failed to add member";
                return false;
            }
        }

        tx.commit();
        return true;
    }


private:
    void fillGroupFromMap(const std::map<std::string, std::string>& row, Group& group) {
        group.group_id = std::stoull(row.at("group_id"));
        group.group_name = row.at("group_name");
        group.group_avatar = row.at("group_avatar");
        group.owner_id = std::stoull(row.at("owner_id"));
        if(row.find("owner_name") != row.end()) {
            group.owner_name = row.at("owner_name");
        }
        group.description = row.at("description");
        group.announcement = row.at("announcement");
        group.max_members = std::stoi(row.at("max_members"));
        group.is_public = std::stoi(row.at("is_public")) == 1;
        group.join_type = std::stoi(row.at("join_type"));
        group.created_at = std::stoll(row.at("created_at"));
        group.updated_at = std::stoll(row.at("updated_at"));
    }


    void fillMemberFromMap(const std::map<std::string, std::string>& row, GroupMember& member) {
        member.member_id = std::stoull(row.at("member_id"));
        member.group_id = std::stoull(row.at("group_id"));
        member.user_id = std::stoull(row.at("user_id"));
        if(row.find("username") !=row.end()) {
            member.username = row.at("username");
        }
        member.nickname = row.at("nickname");
        member.role = std::stoi(row.at("role"));
        member.join_time = std::stoll(row.at("join_time"));
        member.last_read_time = std::stoll(row.at("last_read_time"));
        member.is_muted = std::stoi(row.at("is_muted")) == 1;
        member.muted_until = std::stoll(row.at("muted_until"));
    }

    void fillRequestFromMap(const std::map<std::string, std::string>& row, GroupJoinRequest& req) {
        req.request_id = std::stoull(row.at("request_id"));
        req.group_id = std::stoull(row.at("group_id"));
        req.from_uid = std::stoull(row.at("from_uid"));
        if(row.find("from_username") != row.end()) {
            req.from_username = row.at("from_username");
        }
        req.message = row.at("message");
        req.status = std::stoi(row.at("status"));
        req.created_at = std::stoll(row.at("created_at"));
        req.updated_at = std::stoll(row.at("updated_at"));
    }

};