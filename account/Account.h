#pragma once

#include "../protobuf/p.h"
#include "../mysql/userDAO.h"
#include "HashSalt.h"
#include "yanzheng.h"
#include "Manager.h"
#include "../logging.h"
#include "../epoll.h"
#include <regex>

class AuthHandler {
    AuthHandler() = default;

    void handleRegister(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::RegisterRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendCommonResponse(conn, header, false, "Invaild request");
            return ;
        }

        LOG_INFO << "Register request from " << request.username();

        // 验证用户名格式
        if(!validateUsername(request.username())) {
            sendCommonResponse(conn, header, false, "Invalid username format (3-20) char, letters/digits/_");
            return ;
        }

        // 检验邮箱
        if(!validataEmail(request.email())) {
            sendCommonResponse(conn, header, false, "Invalid email format");
            return ;
        }

        // 验证密码
        if(!validataPassword(request.password())) {
            sendCommonResponse(conn, header, false, "Password too weak");
            return ;
        }

        // 验证验证码
        std::string key = "register_" + request.email();
        if(!YanZheng::getInstance().verifycode(key, request.verification_code())) {
            sendCommonResponse(conn, header, false, "Invalid or expired verifycode");
            return ;
        }

        // 用户名称唯一
        UserDAO user_dao;
        USER existing_user;
        if(user_dao.getUserByUsername(request.username(), existing_user)) {
            sendCommonResponse(conn, header, false, "Username exists");
            return ;
        }

        // 验证邮箱唯一
        if(!request.email().empty()) {
            USER user;
            if(user_dao.getUserByEmail(request.email(), user)) {
                sendCommonResponse(conn, header, false, "Email already registered");
                return ;
            }
        }
        

        // 创建账户
        USER user;
        user.username = request.username();
        user.email = request.email();
        user.nickname = request.nickname().empty() ? request.username() : request.nickname();
        user.status = 0;
        user.creat_at = tool::getTimestamp();
        user.update_at = tool::getTimestamp();

        // 生成盐值并加密
        std::string salt = Crypot::generateSalt(16);
        user.passward_hash = Crypot::encryptPassword(request.password(), salt);

        // 存储salt和hash
        user.salt = salt;

        uint64_t new_user_id;
        if(!user_dao.createUser(user, new_user_id)) {
            LOG_ERROR << "Failed to Create User";
            sendCommonResponse(conn, header, true, "Register success");
            return ;
        }

        LOG_INFO << "User register: " << request.username() << "id:" << new_user_id;
        sendCommonResponse(conn, header, true, "Registeration successful");
        
    }

private:
    void sendCommonResponse(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, bool success, const std::string& msg)  {
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

    // 检验名称
    bool validateUsername(const std::string& username) {
        if(username.length() < 3 || username.length() > 20) {
            return false;
        }

        std::regex pattern("^[a-zA-Z0-9]+$");
        return std::regex_match(username, pattern);
    }

    // 检验邮箱
    bool validataEmail(const std::string& email) {
        if(email.empty()) {
            return true;
        }
        std::regex pattern(R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})");

        return std::regex_match(email, pattern);
    }

    // 检查密码
    bool validataPassword(const std::string& password) {
        if(password.length() < 8) {
            return false;
        }
        bool has_letter = false, has_digit = false;
        for(char c : password) {
            if(!isdigit(c)) {
                has_digit = true;
            }
            if(!isalpha(c)) {
                has_letter = true;
            }
        }

        return has_letter && has_digit;
    }

};