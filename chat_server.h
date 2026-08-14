#pragma once

#include "reactor.h"
#include "protobuf/p.h"
#include "mysql/userDAO.h"
#include "mysql/friendDAO.h"
#include "mysql/messageDAO.h"
#include "mysql/pingbiDAO.h"
#include "JSON/Config.h"
#include "logging.h"
#include "thread_pool.h"
#include "chat/chathandle.h"
#include "friend/FriendHandle.h"
#include "friend/OnlineManager.h"
#include "group/grouphandle.h"
#include "account/Account.h"
#include <memory>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include "mysql/groupmessageDAO.h"
#include "chat/groupmessagehandle.h"
#include "file/FileHandle.h"
#include "HeartBeat/heartbeat.h"
#include "limiter.h"
#include "deduplicator.h"
#include "account/HashSalt.h"

class ChatServer {
public:
    ChatServer(const std::string& host, uint16_t port, int sub_reactor = 4)
        : host_(host),
          port_(port),
          sub_reactor_(sub_reactor),
          running_(false) {
        
        // 从配置文件中读取线程数
        int threads = Config::getInstance().getInt("server.threads", 8);
        thread_pool_ = std::make_unique<ThreadPool>(threads);
        LOG_INFO << "Thread pool created with " << threads << " threads";

        // 创建 MainReactor
        main_reactor_ = std::make_unique<MainReactor>(host_, port_, sub_reactor_, thread_pool_.get());

        // 设置消息回调
        main_reactor_->setMessageCallback(
            [this](std::shared_ptr<TcpConnection> conn, Buffer& buffer) {
                handleMessage(conn, buffer);
            });

        // 1. ChatHandler
        chat_handler_.setUserConnections(&user_connections_);
        chat_handler_.setFriendDAO(&friend_dao_);
        chat_handler_.setBlockDAO(&block_dao_);

        // 2. FriendHandler
        friend_handler_.setUserConnections(&user_connections_);
        friend_handler_.setBlockDAO(&block_dao_);

        // 3. GroupHandler
        group_handler_.setUserConnections(&user_connections_);

        // 4. FileHandler
        file_handler_.setUserConnections(&user_connections_);

        // 注册业务处理器
        registerHandlers();

        LOG_INFO << "ChatServer initialized on " << host_ << ":" << port_;
    }

    ~ChatServer() {
        stop();
    }

    void start() {
        if (running_) return;
        running_ = true;

        // 启动 MainReactor
        main_reactor_->start();

        // 设置 FriendHandler 的连接获取器
        friend_handler_.setConnection(
            [this](uint64_t uid) -> std::shared_ptr<TcpConnection> {
                auto it = user_connections_.find(uid);
                if (it != user_connections_.end()) {
                    return it->second;
                }
                return nullptr;
            });

        // 启动心跳超时检测线程
        timeout_thread_ = std::thread([this]() {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                OnlineManager::getInstance().checkTimeout(30);
            }
        });

        LOG_INFO << "ChatServer started on " << host_ << ":" << port_;
    }

    void stop() {
        if (!running_) return;
        running_ = false;

        if (timeout_thread_.joinable()) {
            timeout_thread_.join();
        }

        if (main_reactor_) {
            main_reactor_->stop();
        }

        if (thread_pool_) {
            thread_pool_.reset();
        }

        LOG_INFO << "ChatServer stopped";
    }

private:
    // 消息处理
    void handleMessage(std::shared_ptr<TcpConnection> conn, Buffer& buffer) {
        uint64_t user_id = conn->getUserID();
        std::string key = "msg_" + std::to_string(user_id);
        if(!msg_limiter_.isallow(key)) {
            LOG_ERROR << "Message rate limit exceeded for user " << user_id;
            sendLimitResponse(conn);
            buffer.costall();
            return ;
        }
        
        while (buffer.readBytes() >= sizeof(uint32_t)) {
            p::MessageHeader header;
            std::vector<char> body;
            size_t consumed = 0;

            // 解码
            std::vector<char> data(buffer.peek(), buffer.peek() + buffer.readBytes());
            if (!proto::MessageCodec::decode(data, consumed, header, body)) {
                break;
            }

            // 移除已处理的数据
            buffer.costBytes(consumed);

            // 分发消息
            dispatcher_.Dispatcher(conn, header, body);
        }
    }

    void sendLimitResponse(std::shared_ptr<TcpConnection> conn) {
        p::CommonResponse resp;
        resp.set_code(-1);
        resp.set_message("limit, please slow down");
        resp.set_timestamp(tool::getTimestamp());

        p::MessageHeader header;
        header.set_msg_type(p::MSG_UNKNOWN);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, resp);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    // 注册所有消息处理器
    void registerHandlers() {
        // 认证相关
        dispatcher_.registerHandle(p::MSG_REGISTER,
            [this](auto conn, auto& header, auto& body) {
                auth_handler_.handleRegister(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_LOGIN,
            [this](auto conn, auto& header, auto& body) {
                handleLogin(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_LOGOUT,
            [this](auto conn, auto& header, auto& body) {
                auth_handler_.handleLogout(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_VERIFICATION_CODE,
            [this](auto conn, auto& header, auto& body) {
                auth_handler_.handleVerifyCode(conn, header, body);
            });

        // 好友相关
        dispatcher_.registerHandle(p::MSG_ADD_FRIEND,
            [this](auto conn, auto& header, auto& body) {
                friend_handler_.handleAddFriend(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_PROCESS_FRIEND_REQUEST,
            [this](auto conn, auto& header, auto& body) {
                friend_handler_.handleProcessFriendRequest(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_FRIEND_LIST,
            [this](auto conn, auto& header, auto& body) {
                friend_handler_.handleGetFriendList(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_DELETE_FRIEND,
            [this](auto conn, auto& header, auto& body) {
                friend_handler_.handleDeleteFriend(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_BLOCK_USER,
            [this](auto conn, auto& header, auto& body) {
                friend_handler_.handleBlockUser(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_UNBLOCK_USER,
            [this](auto conn, auto& header, auto& body) {
                friend_handler_.handleUnblockUser(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_BLOCK_LIST,
            [this](auto conn, auto& header, auto& body) {
                friend_handler_.handleGetBlockList(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_FRIEND_ONLINE_STATUS,
            [this](auto conn, auto& header, auto& body) {
                friend_handler_.handleOnlineStatus(conn, header, body);
            });

        // 群组相关
        dispatcher_.registerHandle(p::MSG_GROUP_CREATE,
            [this](auto conn, auto& header, auto& body) {
                group_handler_.handleCreateGroup(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_GROUP_DISMISS,
            [this](auto conn, auto& header, auto& body) {
                group_handler_.handleDismissGroup(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_GROUP_JOIN,
            [this](auto conn, auto& header, auto& body) {
                group_handler_.handleJoinGroup(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_GROUP_LEAVE,
            [this](auto conn, auto& header, auto& body) {
                group_handler_.handleLeaveGroup(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_GROUP_LIST,
            [this](auto conn, auto& header, auto& body) {
                group_handler_.handleGetGroupList(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_GROUP_MEMBERS,
            [this](auto conn, auto& header, auto& body) {
                group_handler_.handleGetGroupMembers(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_SET_ADMIN,
            [this](auto conn, auto& header, auto& body) {
                group_handler_.handleSetAdmin(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_KICK_MEMBER,
            [this](auto conn, auto& header, auto& body) {
                group_handler_.Kickmember(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_PENDING_REQUESTS,
            [this](auto conn, auto& header, auto& body) {
                group_handler_.handleGetPendingRequests(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_PROCESS_REQUEST,
            [this](auto conn, auto& header, auto& body) {
                group_handler_.handleProcessJoinRequest(conn, header, body);
            });

        // 聊天相关
        dispatcher_.registerHandle(p::MSG_CHAT,
            [this](auto conn, auto& header, auto& body) {
                chat_handler_.handleChat(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_HISTORY,
            [this](auto conn, auto& header, auto& body) {
                chat_handler_.handleGethistory(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_READ_RECEIPT,
            [this](auto conn, auto& header, auto& body) {
                chat_handler_.handleMarkRead(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_UNREAD_COUNT,
            [this](auto conn, auto& header, auto& body) {
                chat_handler_.handleGetUnreadCount(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_CONVERSATION_LIST,
            [this](auto conn, auto& header, auto& body) {
                chat_handler_.handleGetConversations(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_RECALL,
            [this](auto conn, auto& header, auto& body) {
                chat_handler_.handleRecallMessage(conn, header, body);
            });


        dispatcher_.registerHandle(p::MSG_GROUP_CHAT,
            [this](auto conn, auto& header, auto& body) {
                groupchat_handler_.handleGroupChat(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_GROUP_HISTORY,
            [this](auto conn, auto& header, auto& body) {
                groupchat_handler_.handleGroupHistory(conn, header, body);
            });
            
        dispatcher_.registerHandle(p::MSG_GROUP_RECALL,
            [this](auto conn, auto& header, auto& body) {
                groupchat_handler_.handleRecallGroupMessage(conn, header, body);
            });
        
        dispatcher_.registerHandle(p::MSG_GROUP_UNREAD,
            [this](auto conn, auto& header, auto& body) {
                groupchat_handler_.handleGroupUnread(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_GROUP_READ,
            [this](auto conn, auto& header, auto& body) {
                groupchat_handler_.handleMarkGroupRead(conn, header, body);
            });

        // 文件相关
        dispatcher_.registerHandle(p::MSG_FILE_UPLOAD_REQ,
            [this](auto conn, auto& header, auto& body) {
                file_handler_.handleFileUploadRequest(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_FILE_UPLOAD_CHUNK,
            [this](auto conn, auto& header, auto& body) {
                file_handler_.handleFileUploadChunk(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_FILE_DOWNLOAD_REQ,
            [this](auto conn, auto& header, auto& body) {
                file_handler_.handleFileDownloadReq(conn, header, body);
            });

        dispatcher_.registerHandle(p::MSG_FILE_RESUME_REQ,
            [this](auto conn, auto& header, auto& body) {
                file_handler_.handleFileResumeReq(conn, header, body);
            });
        
        dispatcher_.registerHandle(p::MSG_FILE_OFFLINE_DOWNLOAD,
            [this](auto conn, auto& header, auto& body) {
                file_handler_.handleOfflineFiles(conn, header, body);
            });
        

        // 通用
        dispatcher_.registerHandle(p::MSG_ECHO,
            [this](auto conn, auto& header, auto& body) {
                handleEcho(conn, header, body);
            });


        // 心跳检测
        dispatcher_.registerHandle(p::MSG_HEARTBEAT,
            [this](auto conn, auto& header, auto& body) {
                if(!req_limiter_.isallow("heartbeat_" + std::to_string(conn->getUserID()))) {
                    LOG_ERROR << "Heartbeat rate limit exceeded for user " << conn->getUserID();
                    return ;
                }
            heartbeat_handler_.handleHeartBeat(conn, header, body);
        });
        
        LOG_INFO << "Registered " << dispatcher_.handlesCount() << " message handlers";
    }

    // 登录处理
    void handleLogin(std::shared_ptr<TcpConnection> conn,
                     const p::MessageHeader& header,
                     const std::vector<char>& body) {
        p::LoginRequest request;
        if (!request.ParseFromArray(body.data(), body.size())) {
            LOG_ERROR << "Parse LoginRequest failed";
            sendResponse(conn, header, false, "Invalid request");
            return;
        }

        LOG_INFO << "Login request from " << request.username();


        UserDAO user_dao;
        USER user;
        bool found = user_dao.getUserByUsername(request.username(), user);

        p::LoginResponse response;
        if (found && Crypot::verifyPassword(request.password(), user.salt, user.password_hash)) {
            response.set_success(true);
            response.set_token("token_" + std::to_string(user.user_id));
            response.set_uid(user.user_id);
            response.set_nickname(user.nickname);

            // 更新用户状态为在线
            user_dao.updateUserStatus(user.user_id, 1);

            // 保存连接映射
            user_connections_[user.user_id] = conn;
            conn->setContext(reinterpret_cast<void*>(user.user_id));

            // 更新心跳
            OnlineManager::getInstance().updateHeartbeat(user.user_id);

            // 发送离线消息
            chat_handler_.sendOfflineMessage(user.user_id);

            LOG_INFO << "User " << user.username << " (" << user.user_id << ") logged in";
        } else {
            response.set_success(false);
            response.set_message("Invalid username or password");
            LOG_ERROR << "Login failed for " << request.username();
        }

        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_LOGIN);
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, response);
        if (!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    // 回声处理
    void handleEcho(std::shared_ptr<TcpConnection> conn,
                    const p::MessageHeader& header,
                    const std::vector<char>& body) {
        p::EchoRequest request;
        if (!request.ParseFromArray(body.data(), body.size())) {
            LOG_ERROR << "Parse EchoRequest failed";
            return;
        }

        p::EchoResponse response;
        response.set_content("Echo: " + request.content());

        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_ECHO);
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, response);
        if (!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    // 通用响应
    void sendResponse(std::shared_ptr<TcpConnection> conn,
                      const p::MessageHeader& header,
                      bool success,
                      const std::string& msg) {
        p::CommonResponse response;
        response.set_code(success ? 0 : -1);
        response.set_message(msg);
        response.set_timestamp(tool::getTimestamp());

        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(header.msg_type());
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, response);
        if (!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    void onUserLogin(uint64_t user_id) {
        if(user_id == 0) {
            LOG_ERROR << "Invalid user_id: 0";
            return ;
        }

        LOG_INFO << "User " << user_id << " login, checking offline messages";

        // 拉取私聊离线消息
        MessageDAO msg_dao;
        auto private_msgs_ = msg_dao.getOfflineMessages(user_id);

        // 拉取群聊离线消息
        GroupMessageDAO group_dao;
        auto group_msgs_ = group_dao.getGroupOfflineMessages(user_id);

        size_t total_private = private_msgs_.size();
        size_t total_group = group_msgs_.size();
        size_t total_count = total_private + total_group;
        
        if(total_count == 0) {
            LOG_DEBUG << "No offline messages for user " << user_id;
            return ;
        }

        LOG_INFO << "User " << user_id << " has " << total_private << " private and " << total_group << " group offline messages";

        // 获取用户连接
        auto conn = getConnection(user_id);
        if(!conn || conn->isClosed()) {
            LOG_ERROR << "User " << user_id << "is offline, cannot deliver offline messages";
            return ;
        }

        // 发送通知
        p::OfflineMessageNotify notify;
        notify.set_private_count(total_private);
        notify.set_group_count(total_group);
        notify.set_total_count(total_count);
        notify.set_timestamp(tool::getTimestamp());
    
        p::MessageHeader header;
        header.set_msg_type(p::MSG_OFFLINE_NOTIFY);
        header.set_timestamp(tool::getTimestamp());
        header.set_to_uid(user_id);

        auto notify_data = proto::MessageCodec::encode(header, notify);
        if(!notify_data.empty()) {
            conn->send(notify_data.data(), notify_data.size());
            LOG_DEBUG << "Offline notify sent to user " << user_id;
        }

        // 发送私聊消息
        size_t sent_private = 0;
        MessageDeduplicator& dedup = MessageDeduplicator::getInstance();
        for(const auto& msg : private_msgs_) {
            if (!conn || conn->isClosed()) break;
            if(dedup.isDuplicate(msg.msg_id)) {
                LOG_DEBUG << "Duplicate private offline msg " << msg.msg_id << ", skip";
                continue;
            }

            dedup.markProcessed(msg.msg_id);

            p::ChatMessage chat_msg;
            chat_msg.set_from_uid(msg.from_uid);
            chat_msg.set_to_uid(msg.to_uid);
            chat_msg.set_content(msg.content);
            chat_msg.set_msg_type(msg.msg_type);
            chat_msg.set_timestamp(msg.created_at);

            p::MessageHeader msg_header;
            msg_header.set_msg_id(msg.msg_id);
            msg_header.set_msg_type(p::MSG_CHAT);
            msg_header.set_timestamp(msg.created_at);
            msg_header.set_to_uid(msg.to_uid);
            msg_header.set_from_uid(msg.from_uid);

            auto data = proto::MessageCodec::encode(msg_header, chat_msg);
            if(!data.empty()) {
                conn->send(data.data(), data.size());
                sent_private++;
            }
            else {
                LOG_ERROR << "Encode private offline msg " << msg.msg_id << " failed";
            }
        }

        // 发送群聊消息
        size_t sent_group = 0;
        for(const auto& msg : group_msgs_) {
            if (!conn || conn->isClosed()) break;
            if(dedup.isDuplicate(msg.msg_id)) {
                LOG_DEBUG << "Duplicate group offline msg " << msg.msg_id << ", skip";
                continue;
            }

            dedup.markProcessed(msg.msg_id);

            p::GroupChatMessage group_msg;
            group_msg.set_group_uid(msg.group_id);
            group_msg.set_from_uid(msg.from_uid);
            group_msg.set_content(msg.content);
            group_msg.set_msg_type(msg.msg_type);
            group_msg.set_timestamp(msg.created_at);

            p::MessageHeader msg_header;
            msg_header.set_msg_id(msg.msg_id);
            msg_header.set_msg_type(p::MSG_GROUP_CHAT);
            msg_header.set_from_uid(msg.from_uid);
            msg_header.set_to_uid(user_id);
            msg_header.set_timestamp(msg.created_at);

            auto data = proto::MessageCodec::encode(msg_header, group_msg);
            if(!data.empty()) {
                conn->send(data.data(), data.size());
                sent_group++;
            }
            else {
                LOG_ERROR << "Encode group offline msg " << msg.msg_id << " failed";
            }
        }

        LOG_INFO << "Offline messages delivered to user " << user_id 
                 << " (private: " << sent_private << "/" << total_private 
                 << ", group: " << sent_group << "/" << total_group << ")";


    }

    std::shared_ptr<TcpConnection> getConnection(uint64_t user_id) {
        auto it = user_connections_.find(user_id);
        if(it != user_connections_.end()) {
            return it->second;
        }

        return nullptr;
    }

    std::string host_;
    uint16_t port_;
    int sub_reactor_;
    std::atomic<bool> running_;

    std::unique_ptr<ThreadPool> thread_pool_;
    std::unique_ptr<MainReactor> main_reactor_;
    std::thread timeout_thread_;

    proto::Dispatch dispatcher_;
    std::unordered_map<uint64_t, std::shared_ptr<TcpConnection>> user_connections_;

    // DAO 实例
    FriendDAO friend_dao_;
    BlockDAO block_dao_;

    // Handler 实例
    AuthHandler auth_handler_;
    friendHandle friend_handler_;
    GroupHandle group_handler_;
    ChatHandle chat_handler_;
    GroupChatHandle groupchat_handler_;
    FileHandle file_handler_;
    HeartbeatHandle heartbeat_handler_;
    
    // 限制
    Limiter& msg_limiter_ = LimiterManage::getInstance().getMessageLimit();
    Limiter& req_limiter_ = LimiterManage::getInstance().getRequestLimit();
};