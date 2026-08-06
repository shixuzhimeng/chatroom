#pragma once

#include <string>
#include <vector>
#include "baseDAO.h"
#include "../tool.h"


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
};



class UserDAO : public BaseDAO{
public:
    bool createUser(const USER& user, uint64_t& user_id) {
        std::string sql = "INSERT INTO users(username, password_hash, salt, email, phone, "
                          "nickname, avatar, status, created_at, updated_at) VALUES ('";
        sql += escapeString(user.username) + "', '";
        sql += escapeString(user.password_hash) + "', '";
        sql += escapeString(user.salt) + "', '";
        sql += escapeString(user.email) + "', '";
        sql += escapeString(user.phone) + "', '";
        sql += escapeString(user.nickname) + "', '";
        sql += escapeString(user.avatar) + "', ";
        sql += std::to_string(user.status) + ", ";
        sql += std::to_string(user.created_at) + ", ";
        sql += std::to_string(user.updated_at) + ")";
        
        if(executeUpdate(sql)) {
            user_id = getLastInserterID();
            return true;
        }

        return false;
    }

    bool getUserByID(uint64_t user_id, USER& user) {
        std::string sql = "SELECT * FROM users WHERE user_id = '" + std::to_string(user_id) + "'";
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
    }
};