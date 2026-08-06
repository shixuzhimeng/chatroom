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
public:
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
        user.created_at = tool::getTimestamp();
        user.updated_at = tool::getTimestamp();

        // 生成盐值并加密
        std::string salt = Crypot::generateSalt(16);
        user.password_hash = Crypot::encryptPassword(request.password(), salt);

        // 存储salt和hash
        user.salt = salt;

        uint64_t new_user_id;
        if(!user_dao.createUser(user, new_user_id)) {
            LOG_ERROR << "Failed to Create User";
            sendCommonResponse(conn, header, false, "Register failed");
            return ;
        }

        LOG_INFO << "User register: " << request.username() << "id:" << new_user_id;
        sendCommonResponse(conn, header, true, "Registeration successful");
        
    }

    // 登录处理
    void handleLogin(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::LoginRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendCommonResponse(conn, header, false, "Invalid request");
            return ;
        }
        LOG_INFO << "LogIn request from " << request.username();
        
        UserDAO user_dao;
        USER user;
        if(!user_dao.getUserByUsername(request.username(), user)) {
            sendCommonResponse(conn, header, false, "User not found");
            return ;
        }

        // 验证密码
        if(!Crypot::verifyPassword(request.password(), user.salt, user.password_hash)) {
            sendCommonResponse(conn, header, false, "Wrong password");
            return ;
        }

        //生成临时身份ID
        std::string devic_id = request.device_id().empty() ? "unknow" : request.device_id();
        std::string token = TManager::getInstance().generateT(user.user_id, user.username, devic_id, 24);
        
        // 更新在线状态
        user_dao.updateUserStatus(user.user_id, 1);

        // 构造登录响应
        p::LoginResponse response;
        response.set_success(true);
        response.set_token(token);
        response.set_message("LogIN success");
        response.set_uid(user.user_id);
        response.set_nickname(user.nickname);

        // 发送响应
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_LOGIN);
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, response);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }

        LOG_INFO << "USER loggend in" << user.username << "( uid= " << user.user_id << " )";
    }

    //注销处理
    void handleLogout(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::LogoutRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendCommonResponse(conn, header, false, "Invalid requset");
            return ;
        }

        std::string token = request.token();
        if(token.empty()) {
            sendCommonResponse(conn, header, false, "Token required");
            return ;
        }

        TManager::TInfo info;
        if(!TManager::getInstance().verifyT(token, info)) {
            sendCommonResponse(conn, header, false, "INvalid token");
            return ;
        }
        
        // 临时身份ID取消
        TManager::getInstance().revoketID(token);
        
        auto tokens = TManager::getInstance().getUserT(info.user_id);
        if(tokens.empty()) {
            UserDAO user_dao;
            user_dao.updateUserStatus(info.user_id, 0);
            LOG_INFO << "User offline " << info.username << "(uid = " << info.user_id << ")"; 
        }

        sendCommonResponse(conn, header, true, "Logout success");
        LOG_INFO << "User LogOut : " << info.username << "(uid = " << info.user_id << ")"; 
    }

    // 验证码请求
    void handleVerifyCode(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::CodeRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendCommonResponse(conn, header, false, "Invalid request");
            return ;
        }

        std::string email = request.email();
        if(!validataEmail(email)) {
            sendCommonResponse(conn, header, false , "Invalid email");
            return ;
        }
        
        std::string key = "register_" + email;
        std::string code = YanZheng::getInstance().generateCode(key, 300);

        p::CodeResponse response;
        response.set_code(code);
        response.set_expire_seconds(300);
        
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_VERIFICATION_CODE);
        resp_header.set_timestamp(tool::getTimestamp());
    
        auto data = proto::MessageCodec::encode(resp_header, response);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }

        LOG_INFO << "Verification code sent to email: " << email;
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
            if(isdigit(c)) {
                has_digit = true;
            }
            if(isalpha(c)) {
                has_letter = true;
            }
        }

        return has_letter && has_digit;
    }

};