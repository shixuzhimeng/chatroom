#pragma once

#include "reactor.h"
#include "protobuf/p.h"
#include "mysql/userDAO.h"
#include "JSON/Config.h"
#include "logging.h"
#include "thread_pool.h"
#include <memory>
#include <unordered_map>
#include <atomic>


class ChatServer{
public:
    ChatServer(const std::string& host , uint16_t port, int sub_reactor = 4) 
    :host_(host),
    port_(port),
    sub_reactor_(sub_reactor)
    {
        // 从配置文件中读取线程数
        int threads = Config::getInstance().getInt("server.threads", 8);
        thread_pool_ = std::make_unique<ThreadPool>(threads);
    
        // 创建Mainreactor
        main_reactor_ = std::make_unique<MainReactor>(host_, port_, sub_reactor_, thread_pool_.get());
        
        // 设置消息回调
        main_reactor_->setMessageCallback(
        [this](std::shared_ptr<TcpConnection> conn, Buffer& buffer) {
            handleMessage(conn, buffer);
        }
    );
    
        //注册业务处理器
        registerHandler();

        LOG_INFO << "ChatServer init on" << host_ << ":" << port_;
    }

    void start() {
        main_reactor_->start();
        LOG_INFO << "ChatServer started";
    }

    void stop() {
        main_reactor_->stop();
        if(thread_pool_) {
            thread_pool_.reset();
        }

        LOG_INFO << "ChatServer stopped";
    }
    

private:
    void handleMessage(std::shared_ptr<TcpConnection> conn, Buffer& buffer) {
        while(buffer.readBytes() >= sizeof(uint32_t)) {
            p::MessageHeader header;
            std::vector<char> body;
            size_t consumed = 0;

            // 解码
            std::vector<char> data(buffer.peek(), buffer.peek() + buffer.readBytes());
            if(!proto::MessageCodec::decode(data, consumed, header, body)) {
                break;
            }

            // 已经处理的数据
            buffer.costBytes(consumed);

            // 分发
            dispatcher_.Dispatcher(conn, header, body);
        }
    }

    void registerHandler() {
        
        // 登录处理
        dispatcher_.registerHandle(p::MSG_LOGIN, [this](auto conn, auto& header, auto& body) {
            handleLogin(conn, header, body);
        });

        // 回声处理
        dispatcher_.registerHandle(p::MSG_ECHO, [this](auto conn, auto& header, auto& body) {
            handleEcho(conn, header, body);
        });

        // 心跳处理
        dispatcher_.registerHandle(p::MSG_HEARTBEAT, [this](auto conn, auto& header, auto& body) {
            handleheartbeat(conn, header, body);
        });

        // 聊天处理
        dispatcher_.registerHandle(p::MSG_CHAT, [this](auto conn, auto& header, auto& body) {
            handleChat(conn, header, body);
        });

        //好友请求处理
        dispatcher_.registerHandle(p::MSG_FRIEND_REQUEST, [this](auto conn, auto& header, auto& body) {
            handlefriendRequest(conn, header, body);
        });

        LOG_INFO << "Registered " << dispatcher_.handlesCount() << "message handle"; 
    }

    void handleLogin(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        // 解析请求
        p::LoginRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            LOG_ERROR << "Prase LoginRequest failed";
            sendResponse(conn, header, false, "Invalid request");
            return ;
        }

        LOG_INFO << "Login request from " << request.username();
        // 数据库查询
        UserDAO user_dao;
        USER user;
        bool found = user_dao.getUserByUsername(request.username(), user);

        // 验证密码
        p::LoginResponse response;
        if(found && user.passward_hash == request.password()) {
            response.set_success(true);
            response.set_token("token_" + std::to_string(user.user_id));
            response.set_uid(user.user_id);
            response.set_nickname(user.nickname);

            // 更新用户状态
            user_dao.updateUserStatus(user.user_id, 1);

            // 保存连接
            user_connections_[user.user_id] = conn;
            conn->setContext(reinterpret_cast<void*>(user.user_id));


            LOG_INFO << "User" << user.username << "(" << user.user_id << ") logged in";
        }
        else {
            response.set_success(false);
            response.set_message("Invalid username or password");
            LOG_ERROR << "Login failed for " << request.username();
        }

        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_LOGIN);
        resp_header.set_timestamp(tool::getTimestamp());
        
        auto data  = proto::MessageCodec::encode(resp_header, response);
        if(data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    void handleEcho(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::EchoRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            LOG_ERROR << "Parse EchoRequest failed";
            return;
        }
        p::EchoResponse response;
        response.set_content("Echo :" + request.content());
    
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_ECHO);
        resp_header.set_timestamp(tool::getTimestamp());
    
        auto data = proto::MessageCodec::encode(resp_header, response);
        if(data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    void handleheartbeat(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::Heartbeat response;
        response.set_timestamp(tool::getTimestamp());
        
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_HEARTBEAT);
        resp_header.set_timestamp(tool::getTimestamp());
    
        auto data = proto::MessageCodec::encode(resp_header, response);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    void handleChat(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::ChatMessage message;
        if(!message.ParseFromArray(body.data(), body.size())) {
            LOG_ERROR << "Parse ChatMessage failed";
            return ;
        }

        LOG_INFO << "Chat from " << message.from_uid() << "to " << message.to_uid() << ": " << message.content();

        auto it = user_connections_.find(message.to_uid());
        if(it != user_connections_.end()) {
            p::MessageHeader resp_header;
            resp_header.set_msg_type(p::MSG_CHAT);
            resp_header.set_timestamp(tool::getTimestamp());
            resp_header.set_from_uid(message.from_uid());
            resp_header.set_to_uid(message.to_uid());
        
            auto data = proto::MessageCodec::encode(resp_header, message);
            if(!data.empty()) {
                it->second->send(data.data(), data.size());
            }        
        }
        else {
            LOG_DEBUG << "User " << message.to_uid()  << "is offline";
        }
    }

    void handlefriendRequest(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::FriendRequest request;
        if(!request.ParseFromArray(body.data(), body.size())) {
            LOG_ERROR << "Parise FriendRequest failed";
            return ;
        }
        LOG_INFO << "Friend request from " << request.from_uid() << "to " << request.from_uid();
        
        p::CommonResponse response;
        response.set_code(0);
        response.set_message("Friend request sent");
        response.set_timestamp(tool::getTimestamp());

        p::MessageHeader resp_header;
        resp_header.set_msg_type(p::MSG_FRIEND_REQUEST);
        resp_header.set_timestamp(tool::getTimestamp());
    
        auto data = proto::MessageCodec::encode(resp_header, response);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    void sendResponse(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, bool success, const std::string& msg) {
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

    std::string host_;
    uint16_t port_;
    int sub_reactor_;
    std::unique_ptr<ThreadPool> thread_pool_;
    std::unique_ptr<MainReactor> main_reactor_;
    proto::Dispatch dispatcher_;
    std::unordered_map<uint64_t, std::shared_ptr<TcpConnection>> user_connections_;
};