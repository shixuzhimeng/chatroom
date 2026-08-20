#pragma once

#include <string>
#include <vector>
#include "baseDAO.h"
#include "../tool.h"
#include "protobuf/mysql_p.h"


struct USER{
    uint64_t user_id = 0;
    std::string username;
    std::string password_hash;
    std::string salt;
    std::string email;
    std::string phone;
    std::string nickname;
    std::string avatar;
    int status = 0;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    std::string settings;  // 存储序列化的UserSetting
};



class UserDAO : public BaseDAO{
public:
    bool createUser(const USER& user, uint64_t& user_id) {
        // 序列化设置
        std::string settings_json = user.settings.empty() ? "{}" : user.settings;
        std::string sql = "INSERT INTO users(username, password_hash, salt, email, phone, "
                          "nickname, avatar, status, settings, created_at, updated_at) VALUES ('";
        sql += escapeString(user.username) + "', '";
        sql += escapeString(user.password_hash) + "', '";
        sql += escapeString(user.salt) + "', '";
        sql += escapeString(user.email) + "', '";
        sql += escapeString(user.phone) + "', '";
        sql += escapeString(user.nickname) + "', '";
        sql += escapeString(user.avatar) + "', ";
        sql += std::to_string(user.status) + ", '";
        sql += escapeString(settings_json) + "', ";
        sql += std::to_string(user.created_at) + ", ";
        sql += std::to_string(user.updated_at) + ")";

        LOG_INFO << "createUser: SQL=" << sql;
        
        if(executeUpdate(sql)) {
            user_id = getLastInsertID();
            return true;
        }

        LOG_ERROR << "createUser: FAILED";
        return false;
    }

    bool getUserByID(uint64_t user_id, USER& user) {
        std::string sql = "SELECT * FROM users WHERE user_id = " + std::to_string(user_id);
        std::vector<std::map<std::string, std::string>> result;
        
        if(!executeQuery(sql, result) || result.empty()) {
            return false;
        }

        fillUserFromMap(result[0], user);
        return true;
    }

    bool getUserByUsername(const std::string& username, USER& user) {
        std::string sql = "SELECT * FROM users WHERE username = '" + escapeString(username) + "'";
        std::vector<std::map<std::string , std::string>> result;

        if(!executeQuery(sql, result) || result.empty()) {
            return false;
        }
        fillUserFromMap(result[0], user);
        return true;
    }

    bool getUserByEmail(const std::string& email, USER& user) {
        std::string sql = "SELECT * FROM users WHERE email = '" + escapeString(email) + "'";
        std::vector<std::map<std::string, std::string>> result;

        if(!executeQuery(sql, result) || result.empty()) {
            return false;
        }

        fillUserFromMap(result[0], user);
        return true;
    }

    bool updateUserStatus(uint64_t user_id, int status) {
        std::string sql = "UPDATE users SET status = " + std::to_string(status) + ", updated_at = " + std::to_string(tool::getTimestamp()) + " WHERE user_id = " + std::to_string(user_id);
        return executeUpdate(sql);
    }

    bool updateUserSettings(uint64_t user_id, const std::string& settings_json) {
        std::string sql = "UPDATE users SET settings = '" + escapeString(settings_json) +
                          "', updated_at = " + std::to_string(tool::getTimestamp()) +
                          " WHERE user_id = " + std::to_string(user_id);
        return executeUpdate(sql);
    }
    
    // 使用Protobuf更新用户设置
    bool updateUserSettings(uint64_t user_id, const db::UserSettings& settings) {
        std::string json = Switch::sToJson(settings);
        return updateUserSettings(user_id, json);
    }
    
    // 获取用户设置（Protobuf格式）
    bool getUserSettings(uint64_t user_id, db::UserSettings& settings) {
        USER user;
        if (!getUserByID(user_id, user)) {
            return false;
        }
        return Switch::dsFromJson(user.settings, settings);
    }

    bool updateUserInfo(const USER& user) {
        std::string sql = "UPDATE users SET ";
        sql += "nickname = '" + escapeString(user.nickname) + "', ";
        sql += "email = '" + escapeString(user.email) + "', ";
        sql += "phone = '" + escapeString(user.phone) + "', ";
        sql += "avatar = '" + escapeString(user.avatar) + "', ";
        sql += "updated_at = " + std::to_string(tool::getTimestamp()) + " ";
        sql += "WHERE user_id = " + std::to_string(user.user_id);

        return  executeUpdate(sql);
    }

    bool deleteUser(uint64_t user_id) {
        std::string sql = "DELETE FROM users WHERE user_id = " + std::to_string(user_id);
        return executeUpdate(sql);
    }

    std::vector<USER> getOnlineUsers() {
        std::vector<USER> users;
        std::string sql = "SELECT * FROM users WHERE status = 1";

        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result)) {
            return users;
        }

        for(const auto& row : result) {
            USER  user;
            fillUserFromMap(row, user);
            users.push_back(user);
        }
        return users;
    }

    // 批量查询
    std::vector<USER> getUsersByIds(const std::vector<uint64_t>& uids) {
        std::vector<USER> result;
        
        // 如果传入的ID列表为空，直接返回空结果
        if (uids.empty()) {
            return result;
        }

        // 构建SQL查询语句
        std::string sql = "SELECT * FROM users WHERE user_id IN (";
        
        for (size_t i = 0; i < uids.size(); ++i) {
            sql += std::to_string(uids[i]);
            if (i + 1 < uids.size()) {
                sql += ", ";
            }
        }
        sql += ")";

        // 查询
        std::vector<std::map<std::string, std::string>> rows;
        if (!executeQuery(sql, rows) || rows.empty()) {
            return result;
        }

        // 将查询结果转换为结构体对象
        for (const auto& row : rows) {
            USER user;
            fillUserFromMap(row, user);
            result.push_back(user);
        }

        return result;
    }

private:
    void fillUserFromMap(const std::map<std::string, std::string>& row, USER& user) {
        user.user_id = std::stoull(row.at("user_id"));
        user.username = row.at("username");
        user.password_hash = row.at("password_hash");
        user.salt = row.at("salt");
        user.email = row.at("email");
        user.phone = row.at("phone");
        user.nickname = row.at("nickname");
        user.avatar = row.at("avatar");
        user.created_at = std::stoll(row.at("created_at"));
        user.updated_at = std::stoll(row.at("updated_at"));
        user.settings = row.at("settings");
    }
};