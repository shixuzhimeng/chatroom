#pragma once

#include "protobuf/p.h"
#include "mysql/userDAO.h"
#include "HashSalt.h"
#include "yanzheng.h"
#include "Manager.h"
#include "tool/logging.h"
#include "net/epoll.h"
#include "Check.h"
#include "limiter.h"
#include "mysql/groupmessageDAO.h"


class AuthHandler {
public:
    AuthHandler() = default;

    void handleRegister(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        LOG_INFO << "handle register";
        p::RegisterRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendCommonResponse(conn, header, false, "Invaild request");
            return ;
        }

        LOG_INFO << "Register request from " << request.username();

        LOG_INFO << "username=[" << request.username() << "]";
        LOG_INFO << "password=[" << request.password() << "]";
        LOG_INFO << "email=[" << request.email() << "]";
        LOG_INFO << "nickname=[" << request.nickname() << "]";
        LOG_INFO << "nickname size=" << request.nickname().size();

        for (unsigned char c : request.nickname()) {
            LOG_INFO << "nickname byte=0x"
                    << std::hex
                    << static_cast<int>(c)
                    << std::dec;
        }

        // 检验输入验证
        auto result = InputValidator::validateRegisterInput(
            request.username(),
            request.password(),
            request.email(),
            request.nickname()
        );

        if(!result.valid) {
            sendCommonResponse(conn, header, false, result.error);
            return ;
        }

        if(InputValidator::hasSQLInjectionRisk(request.username()) || InputValidator::hasSQLInjectionRisk(request.email())) {
            sendCommonResponse(conn, header, false, "Invalid input");
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
        
        std::string key = "logon_" + request.username();
        if(!LimiterManage::getInstance().getLoginLimit().isallow(key)) {
            sendCommonResponse(conn, header, false, "Login attempts too frequent");
            return ;
        }

        UserDAO user_dao;
        USER user;
        if(!user_dao.getUserByUsername(request.username(), user)) {
            sendCommonResponse(conn, header, false, "User not found");
            return ;
        }

        if(!Crypot::verifyPassword(request.password(), user.salt, user.password_hash)) {
            sendCommonResponse(conn, header, false, "Wrong password");
            return ;
        }

        OnlineManager::getInstance().removeUser(user.user_id);
        
        user_dao.updateUserStatus(user.user_id, 1);

        std::string device_id = request.device_id().empty() ? "unknown" : request.device_id();
        std::string token = TManager::getInstance().generateT(user.user_id, user.username, device_id, 24);
        
        OnlineManager::getInstance().userOnline(user.user_id);

        // 构造登录响应
        p::LoginResponse response;
        response.set_success(true);
        response.set_token(token);
        response.set_message("Login success");
        response.set_uid(user.user_id);
        response.set_nickname(user.nickname);

        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_LOGIN);
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, response);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }

        LOG_INFO << "User logged in " << user.username << " (uid=" << user.user_id << ")";
    }

    //注销处理
    void handleLogout(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::LogoutRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendCommonResponse(conn, header, false, "Invalid requset");
            return ;
        }

        uint64_t user_id = conn->getUserID();
        if (user_id != 0) {
            OnlineManager::getInstance().userOffline(user_id);
        }

        std::string token = request.token();
        if(token.empty()) {
            sendCommonResponse(conn, header, false, "Token required");
            return ;
        }

        TManager::TInfo info;
        if(!TManager::getInstance().verifyT(token, info)) {
            sendCommonResponse(conn, header, false, "Invalid token");
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

    // 注销账户处理
    void handleDeleteAccount(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::DeleteAccountRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendCommonResponse(conn, header, false, "Invalid requset", p::MSG_DELETE_ACCOUNT);
            return ;
        }

        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendCommonResponse(conn, header, false, "User not logged in", p::MSG_DELETE_ACCOUNT);
            return ;
        }

        std::string TID = request.token();
        if(TID.empty()) {
            sendCommonResponse(conn, header, false, "TID required", p::MSG_DELETE_ACCOUNT);
            return ;
        }

        TManager::TInfo info;
        if(!TManager::getInstance().verifyT(TID, info)) {
            sendCommonResponse(conn, header, false, "Invalid token", p::MSG_DELETE_ACCOUNT);
            return ;
        }

        if(!request.confirm()) {
            sendCommonResponse(conn, header, false, "Invalid required", p::MSG_DELETE_ACCOUNT);
            return ;
        }

        if(!request.password().empty()) {
            UserDAO user_dao;
            USER user;
            if(!user_dao.getUserByID(info.user_id, user)) {
                sendCommonResponse(conn, header, false, "User not found", p::MSG_DELETE_ACCOUNT);
                return ;
            }
            if(!Crypot::verifyPassword(request.password(), user.salt, user.password_hash)) {
                sendCommonResponse(conn, header, false, "Invalid password", p::MSG_DELETE_ACCOUNT);
                return ;
            }
        }

        try {
            FriendDAO friend_dao;
            GroupDAO group_dao;
            MessageDAO msg_dao;
            GroupMessageDAO g_msg_dao;
            UserDAO user_dao;

            TransactionGuard tx(user_dao);
            friend_dao.deleteAllFriend(info.user_id);
            group_dao.deleteAllgroup(info.user_id);
            msg_dao.deleteAllMessage(info.user_id);
            msg_dao.deleteAllOfflineMessages(info.user_id);
            g_msg_dao.deleteMessagesGroup(info.user_id);

            if(!user_dao.deleteUser(info.user_id)) {
                throw std::runtime_error("delete user failed");
            }

            TManager::getInstance().revokeAlltID(info.user_id);

            tx.commit();
        }
        catch(const std::exception& e) {
            LOG_ERROR << "Delete account failed";
            sendCommonResponse(conn, header, false, "Deleted failed", p::MSG_DELETE_ACCOUNT);
            return ;
        }

        p::CommonResponse response;
        response.set_code(0);
        response.set_message("Account deleted success");
        response.set_timestamp(tool::getTimestamp());

        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_DELETE_ACCOUNT);
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, response);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }

        LOG_INFO << "User account deleted: " << info.username << " (uid = " << info.user_id << " )";
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

    // 验证码登录
    void handleVerifyCodeLogin(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::VerifyCodeLoginRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            sendCommonResponse(conn, header, false, "Invalid request");
            return ;
        }

        std::string email = request.email();
        std::string code = request.code();
        std::string device_id = request.device_id();

        // 验证邮箱格式
        if(!validataEmail(email)) {
            sendCommonResponse(conn, header, false, "Invalid email");
            return ;
        }

        // 验证验证码
        std::string key = "register_" + email;
        std::string shored_code = YanZheng::getInstance().generateCode(key);
        if(shored_code.empty()) {
            sendCommonResponse(conn, header, false, "Verifycation code expried");
            return ;
        }

        if(YanZheng::getInstance().verifycode(shored_code, code)) {
            sendCommonResponse(conn, header, false, "Invalid verify code");
            return ;
        }

        YanZheng::getInstance().cleanExpired();

        UserDAO user_dao;
        USER  user;

        if (!user_dao.getUserByEmail(email, user)) {
            // 邮箱未注册，自动创建账号
            user.username = email;
            user.email = email;
            user.nickname = email.substr(0, email.find('@'));
            user.status = 0;
            user.created_at = tool::getTimestamp();
            user.updated_at = tool::getTimestamp();
            user.salt = Crypot::generateSalt(16);
            user.password_hash = "";  // 验证码登录没有密码
            
            uint64_t new_user_id;
            if (!user_dao.createUser(user, new_user_id)) {
                LOG_ERROR << "Failed to create user for email: " << email;
                sendCommonResponse(conn, header, false, "Login failed");
                return;
            }
            user.user_id = new_user_id;
            LOG_INFO << "Auto created user for email: " << email;
        }
        
        // 生成 token
        std::string token = TManager::getInstance().generateT(
            user.user_id, 
            user.username, 
            device_id.empty() ? "unknown" : device_id, 
            24
        );
        
        // 更新在线状态
        user_dao.updateUserStatus(user.user_id, 1);
        
        // 构造登录响应
        p::LoginResponse response;
        response.set_success(true);
        response.set_token(token);
        response.set_message("Login success");
        response.set_uid(user.user_id);
        response.set_nickname(user.nickname);
        
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_LOGIN);
        resp_header.set_timestamp(tool::getTimestamp());
        
        auto data = proto::MessageCodec::encode(resp_header, response);
        if (!data.empty()) {
            conn->send(data.data(), data.size());
        }
        
        LOG_INFO << "User logged in via verification code: " << email 
                 << " (uid=" << user.user_id << ")";
    }


private:
    void sendCommonResponse(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, bool success, const std::string& msg, p::MessageType type = p::MSG_COMMON_RESPONSE) {
        LOG_INFO << "sendCommonResponse: success=" << success << ", msg=" << msg;
        
        p::CommonResponse response;
        response.set_code(success ? 0 : -1);
        response.set_message(msg);
        response.set_timestamp(tool::getTimestamp());

        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(type);
        resp_header.set_timestamp(tool::getTimestamp());
        
        LOG_INFO << "sendCommonResponse: encoding...";
        
        auto data = proto::MessageCodec::encode(resp_header, response);
        if (!data.empty()) {
            LOG_INFO << "sendCommonResponse: sending " << data.size() << " bytes";
            conn->send(data.data(), data.size());
            LOG_INFO << "sendCommonResponse: sent";
        } else {
            LOG_ERROR << "sendCommonResponse: encode failed";
        }
    }

    // 检验名称
    bool validateUsername(const std::string& username) {
        return InputValidator::validateUsername(username);
    }

    // 检验邮箱
    bool validataEmail(const std::string& email) {
        return InputValidator::validateEmail(email);
    }

    // 检查密码
    bool validataPassword(const std::string& password) {
        return InputValidator::validatePassword(password);
    }

};