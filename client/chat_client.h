#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <map>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <filesystem>
#include <cctype>
#include <unordered_map>
#include "TLSclient.h"
#include "protobuf/p.h"
#include "protobuf/mysql_p.h"
#include "tool/tool.h"
#include "file/md5.h"

struct UserInfo {
    uint64_t uid = 0;
    std::string username;
    std::string nickname;
    std::string tID;
    bool logged_in = false;
};

struct ConversationInfo {
    uint64_t user_id = 0;
    std::string username;
    std::string nickname;
    std::string avatar;
    std::string last_msg_content;
    uint64_t last_msg_time = 0;
    int unread_count = 0;
    int online_status = 0;  // 0=离线, 1=在线
};

// 断点续传
struct ResumeResult {
    bool success = false;
    uint64_t offset = 0;
    int error_code = 0;
};

struct UploadMetaResult {
    bool success = false;
    bool is_duplicate = false;
    std::string file_id;
    std::string existing_file_id;
    uint64_t resume_offset = 0;
    std::string error_msg;
};

class ChatClient {
public:
    using MessageCallback = std::function<void(const std::string&)>;
    using UICallback = std::function<void(const std::string& action)>;
    ChatClient() {
        client_ = std::make_unique<TLSClient>();
        client_->setMessageCallback([this](const std::vector<char>& data) {
            handleMessages(data);
        });
    }

    ~ChatClient() {
        disconnect();
    }

    bool connect(const std::string& host, uint16_t port, bool use_tls, const std::string cert_file, const std::string& key_file) {
        host_ = host;
        port_ = port;
        use_tls_ = use_tls;
        cert_file_ = cert_file;
        key_file_ = key_file;
        if(client_->connect(host, port, use_tls, cert_file, key_file)) {
            connected_ = true;
            running = true;
            recv_thread_ = std::thread(&ChatClient::receiveloop, this);
            //startPreLoginHeartbeat();
            return true;
        }
        return false;
    }

    // 断线重连（连接被服务端超时关闭后，再次操作前自动重连）
    bool reconnect() {
        if(client_->isConnected()) {
            return true;
        }
        if(host_.empty()) {
            return false;
        }
        // 清理旧的接收线程
        if(recv_thread_.joinable()) {
            recv_thread_.join();
        }
        if(client_->connect(host_, port_, use_tls_, cert_file_, key_file_)) {
            connected_ = true;
            running = true;
            recv_thread_ = std::thread(&ChatClient::receiveloop, this);
            //startPreLoginHeartbeat();
            LOG_INFO << "Reconnected to " << host_ << ":" << port_;
            return true;
        }
        return false;
    }

    void disconnect() {
        cleanChatMod();
        connected_ = false;
        running = false;
        if(recv_thread_.joinable()) {
            recv_thread_.join();
        }
        //stopHeartbeat();
        //stopPreLoginHeartbeat();
        client_->disconnect();
        user_.logged_in = false;
    }

    // void startPreLoginHeartbeat() {
    //     if (pre_login_heartbeat_running_) {
    //         return;
    //     }
    //     pre_login_heartbeat_running_ = true;
    //     pre_login_heartbeat_thread_ = std::thread([this]() {
    //         std::unique_lock<std::mutex> lock(pre_login_mutex_);
    //         while (pre_login_heartbeat_running_ && !user_.logged_in && client_->isConnected()) {
    //             // 可被 stopPreLoginHeartbeat 唤醒，避免关闭时 join 阻塞 10 秒
    //             pre_login_cv_.wait_for(lock, std::chrono::seconds(10), [this] {
    //                 return !pre_login_heartbeat_running_;
    //             });
    //             if (!pre_login_heartbeat_running_) {
    //                 break;
    //             }
    //             lock.unlock();
    //             if (!user_.logged_in && client_->isConnected()) {
    //                 sendPreLoginHeartbeat();
    //             }
    //             lock.lock();
    //         }
    //         LOG_DEBUG << "Pre-login heartbeat thread exiting";
    //     });
    // }

    // void stopPreLoginHeartbeat() {
    //     {
    //         std::lock_guard<std::mutex> lock(pre_login_mutex_);
    //         pre_login_heartbeat_running_ = false;
    //     }
    //     pre_login_cv_.notify_all();
    //     if (pre_login_heartbeat_thread_.joinable()) {
    //         pre_login_heartbeat_thread_.join();
    //     }
    // }

    // void sendPreLoginHeartbeat() {
    //     if(!client_->isConnected()) {
    //         return;
    //     }
        
    //     p::Heartbeat hb;
    //     hb.set_timestamp(tool::getTimestamp());
    //     hb.set_client_time(tool::getTimestamp());
    //     hb.set_seq(0);  // seq=0 表示登录前心跳

    //     p::MessageHeader header;
    //     header.set_msg_type(p::MSG_HEARTBEAT);
    //     header.set_timestamp(tool::getTimestamp());

    //     auto data = proto::MessageCodec::encode(header, hb);
    //     if(!data.empty()) {
    //         sendData(data);
    //         LOG_DEBUG << "Pre-login heartbeat sent";
    //     }
    // }


    void receiveloop() {
        LOG_DEBUG << "receiveloop start";
        if(!client_ || !client_->isConnected()) {
            return ;
        }

        epoll_fd_ = epoll_create1(0);
        if(epoll_fd_ < 0) {
            LOG_ERROR << "epoll_create failed";
            return ;
        }

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = client_->getfd();
        if(epoll_ctl(epoll_fd_ , EPOLL_CTL_ADD, client_->getfd(), &ev) < 0) {
            LOG_ERROR << "epoll_ctl add failed";
            close(epoll_fd_);
            epoll_fd_ = -1;
            return ;
        }

        struct epoll_event events[10];
        while(running) {
            int n = epoll_wait(epoll_fd_, events, 10, 1000);
            if(n < 0) {
                if(errno == EINTR) {
                    // 信号打断不应终止接收循环
                    continue;
                }
                LOG_ERROR << "epoll_wait error: " << strerror(errno);
                break;
            }
            if(n == 0) {
                if(!client_->isConnected()) {
                    LOG_INFO << "Connected lost, stopping received loop";
                    break;
                }
                continue;
            }

            for(int i = 0; i < n; ++i) {
                if(events[i].data.fd == client_->getfd()) {
                    client_->handleRead();
                }
            }
        }

        if(epoll_fd_ > 0) {
            close(epoll_fd_);
            epoll_fd_ = -1;
        }

        // 连接被动断开（如被服务端单点登录踢下线）时，标记离线并通知 UI
        connected_ = false;
        if(user_.logged_in) {
            user_.logged_in = false;
            if(msg_callback_) {
                msg_callback_("[系统] 与服务器的连接已断开，请重新登录");
            }
        }

        LOG_INFO << "Receive loop exited";
    }

    bool isConnected() const { 
        return connected_;
    }
    
    bool isLoggedIn() const {
        return user_.logged_in;
    }
    
    uint64_t getUserId() const {
        return user_.uid;
    }

    void handleMessages(const std::vector<char>& data) {
        LOG_DEBUG << "handleMessage called";
        p::MessageHeader header;
        std::vector<char> body;
        size_t consumed = 0;
        if(!proto::MessageCodec::decode(data, consumed, header, body)) {
            LOG_ERROR << "Decode failed";
            return ;
        }

        switch(header.msg_type()) {
            case p::MSG_LOGIN:
            {
                p::LoginResponse resp;
                if(resp.ParseFromArray(body.data(), body.size())) {
                    handleLoginResponse(resp);
                }
                break;
            }
            case p::MSG_REGISTER:
            {
                p::CommonResponse resp;
                if(resp.ParseFromArray(body.data(), body.size())) {
                    handleRegisterResponse(resp);
                }
                break;
            }
            case p::MSG_CHAT:
            {
                p::ChatMessage msg;
                if (msg.ParseFromArray(body.data(), body.size())) {
                    LOG_DEBUG << "Received chat message: from=" << msg.from_uid() 
                            << ", to=" << msg.to_uid() 
                            << ", content=" << msg.content();
                    handlePrivateChat(msg);
                }
                break;
            }
            case p::MSG_GROUP_CREATE:
            {
                // 创建群组响应（携带 group_id）
                p::CreateGroupResponse createResp;
                if (createResp.ParseFromArray(body.data(), body.size())) {
                    if (msg_callback_) {
                        if (createResp.success()) {
                            std::string msg = "\n 群组创建成功！";
                            msg += "\n   群组ID: " + std::to_string(createResp.group_id());
                            if (!createResp.message().empty()) {
                                msg += "\n   消息: " + createResp.message();
                            }
                            msg_callback_(msg);
                        } else {
                            msg_callback_("[失败] 群组创建失败: " + createResp.message());
                        }
                    }
                    getGroupList();
                } else {
                    LOG_ERROR << "Failed to parse CreateGroupResponse";
                }
                break;
            }
            case p::MSG_GROUP_NOTIFICATION:
            {
                LOG_DEBUG << "Processing MSG_GROUP_NOTIFICATION (type=28)";
                LOG_DEBUG << "body.size() = " << body.size();
                
                // 尝试解析为 GroupNotification
                p::GroupNotification notify;
                if (notify.ParseFromArray(body.data(), body.size())) {
                    LOG_DEBUG << "Parsed as GroupNotification: " << notify.message();
                    if (msg_callback_) {
                        msg_callback_("[群组通知] " + notify.message());
                    }
                    break;
                }
                
                // 尝试解析为 CommonResponse
                p::CommonResponse commonResp;
                if (commonResp.ParseFromArray(body.data(), body.size())) {
                    LOG_DEBUG << "Parsed as CommonResponse: code=" << commonResp.code() 
                            << ", message=" << commonResp.message();
                    if (msg_callback_) {
                        if (commonResp.code() == 0) {
                            msg_callback_("[成功] " + commonResp.message());
                        } else {
                            msg_callback_("[失败] " + commonResp.message());
                        }
                    }
                    getGroupList();
                    break;
                }
                
                LOG_ERROR << "Failed to parse type=28 message, body size=" << body.size();
                break;
            }
            case p::MSG_GROUP_CHAT: {
                // 尝试用于在线消息
                p::GroupMessagePush online_push;
                if (online_push.ParseFromArray(body.data(), body.size())) {
                    LOG_DEBUG << "=== Direct field values ===";
                    LOG_DEBUG << "msg_id: " << online_push.msg_id();
                    LOG_DEBUG << "group_id: " << online_push.group_id();
                    LOG_DEBUG << "from_uid: " << online_push.from_uid();
                    LOG_DEBUG << "from_username: " << online_push.from_username();
                    LOG_DEBUG << "content: " << online_push.content();
                    LOG_DEBUG << "msg_type: " << online_push.msg_type();
                    LOG_DEBUG << "created_at: " << online_push.created_at();
                    
                    LOG_DEBUG << "DebugString: " << online_push.DebugString();
                    LOG_DEBUG << "Parsed as GroupMessagePush (online message)";
                    handleGroupPush(online_push);
                    break; // 解析成功，跳出
                }

                // 如果第一种失败，再尝试用于离线消息
                p::GroupChatMessage msg;
                if (msg.ParseFromArray(body.data(), body.size())) {
                    LOG_DEBUG << "Parsed GroupChatMessage:";
                    LOG_DEBUG << "  from_uid: " << msg.from_uid();
                    LOG_DEBUG << "  group_uid: " << msg.group_uid();
                    LOG_DEBUG << "  content: " << msg.content();
                    handleGroupChatMessage(msg);
                    break; // 解析成功，跳出
                }

                // 如果两种都失败，说明数据有问题
                LOG_ERROR << "Failed to parse MSG_GROUP_CHAT with both GroupMessagePush and GroupChatMessage";
                break;
            }
            case p::MSG_FRIEND_LIST:
            {
                p::FriendListResponse resp;
                if (resp.ParseFromArray(body.data(), body.size())) {
                    if (msg_callback_) {
                        int count = resp.friends_size();
                        if(count == 0) {
                            msg_callback_("好友列表：（0人）");
                            break;
                        }
                        else {
                            std::string out = "好友列表 (" + std::to_string(resp.friends_size()) + "人):";
                            for (int i = 0; i < count; ++i) {
                                const auto& f = resp.friends(i);
                                out += "\n " + std::to_string(f.user_id()) + " " + f.nickname() + 
                                    (f.is_online() ? " [在线]" : " [离线]");
                            }
                            msg_callback_(out);
                        }
                    }
                }
                break;
            }
            case p::MSG_GROUP_DISMISS:
            {
                LOG_DEBUG << "Processing MAG_GROUP_DISMISS";
                p::CommonResponse resp;
                if(resp.ParseFromArray(body.data(), body.size())) {
                    if (msg_callback_) {
                        if (resp.code() == 0) {
                            msg_callback_("[成功] 群组已解散");
                        }
                        else {
                            msg_callback_("[失败] 解散群组失败: " + resp.message());
                        }
                    }
                    // 刷新群组列表
                    getGroupList();
                }
                else {
                    LOG_ERROR << "Failed to parse CommonResponse for MSG_GROUP_DISMISS";
                    if (msg_callback_) {
                        msg_callback_("[错误] 解析解散群组响应失败");
                    }
                }
                break;
            }
            case p::MSG_GROUP_LIST:
            {
                LOG_DEBUG << "Processing group list (type=22)";
                p::GroupListResponse resp;
                if (resp.ParseFromArray(body.data(), body.size())) {
                    LOG_DEBUG << "Parsed: groups_size=" << resp.groups_size()
                            << ", success=" << resp.success();
                    int count = resp.groups_size();
                    for (int i = 0; i < count; ++i) {
                        const auto& g = resp.groups(i);
                        LOG_DEBUG << "  Group[" << i << "]: id=" << g.group_id() 
                                << ", name='" << g.group_name() << "'"
                                << ", owner=" << g.owner_id()
                                << ", member_count=" << g.member_count()
                                << ", is_public=" << g.is_public();
                    }

                    if (msg_callback_) {
                        int count = resp.groups_size();
                        if (count == 0) {
                            msg_callback_("群聊列表: (0个)");
                        } else {
                            std::ostringstream oss;
                            oss << "我的群聊 (" << count << "个):";
                            for(int i = 0; i < count; ++i) {
                                const auto& g = resp.groups(i);
                                std::string name = g.group_name();
                                if(name.empty()) {
                                    name = "未命名群组";
                                }
                                oss << "\n "  << name << "(ID:" << g.group_id() << ")";
                            }
                            std::string output = oss.str();
                            LOG_DEBUG << "Group list output: " << output;
                            msg_callback_(output);
                        }
                    }
                } else {
                    LOG_ERROR << "Failed to parse GroupListResponse";
                    if (msg_callback_) {
                        msg_callback_("[错误] 解析群组列表失败");
                    }
                }
                break;
            }
            case p::MSG_GROUP_MEMBERS:
            {
                p::GroupMembersResponse resp;
                if(resp.ParseFromArray(body.data(), body.size())) {
                    if(msg_callback_) {
                        int count = resp.members_size();
                        if (count == 0) {
                            msg_callback_("群成员: (0人)");
                        } else {
                            std::string out = "群成员 (" + std::to_string(count) + "人):";
                            for (int i = 0; i < count; ++i) {
                                const auto& m = resp.members(i);
                                out += "\n  " + std::to_string(m.user_id()) + " " + m.nickname();
                            }
                            msg_callback_(out);
                        }
                    }
                }
                break;
            }
            case p::MSG_PENDING_REQUESTS:
            {
                p::PendingRequestsResponse resp;
                if(resp.ParseFromArray(body.data(), body.size())) {
                    if(msg_callback_) {
                        int count = resp.requests_size();
                        if (count == 0) {
                            msg_callback_("待处理的入群申请: (0条)");
                        } else {
                            std::string out = "待处理的入群申请 (" + std::to_string(count) + "条):";
                            for (int i = 0; i < count; ++i) {
                                const auto& r = resp.requests(i);
                                std::string uname = r.from_username();
                                if (uname.empty()) {
                                    uname = std::to_string(r.from_uid());
                                }
                                out += "\n  请求ID: " + std::to_string(r.request_id()) +
                                       ", 来自: " + uname + " (ID:" + std::to_string(r.from_uid()) + ")";
                                if (!r.message().empty()) {
                                    out += ", 附言: " + r.message();
                                }
                                out += " — 使用 /approve " + std::to_string(r.request_id()) + " true/false 处理";
                            }
                            msg_callback_(out);
                        }
                    }
                }
                break;
            }
            case p::MSG_HISTORY:
            {
                p::HistoryResponse resp;
                if(resp.ParseFromArray(body.data(), body.size())) {
                    if(msg_callback_) {
                        std::string out = "历史消息:";
                        for(auto& m : resp.messages()) {
                            uint64_t from = m.from_uid();
                            std::string from_str = (from == user_.uid) ? "我" : std::to_string(from);

                            uint64_t time = m.created_at();
                            std::string time_str = std::to_string(time);

                            if(m.msg_type() == 3) {
                                // 文件消息
                                out += "\n [" + time_str +  "]" + from_str + ": [文件] " + m.content() +
                                       " (ID: " + m.file_id() + ")";
                            }
                            else {
                                std::string content = m.content();
                                if(content.empty()) {
                                    content = "空消息";
                                }
                                uint64_t msg_id = m.msg_id();
                                out += "\n [" + time_str +  "]" + from_str + ":" + content + "(ID: " + std::to_string(msg_id) + ")";
                            }
                        }
                        msg_callback_(out);
                    }
                }
                break;
            }
            case p::MSG_GROUP_HISTORY:
            {
                p::GroupHistoryResponse resp;
                if(resp.ParseFromArray(body.data(), body.size())) {
                    if(msg_callback_) {
                        std::string out = "群组历史消息";
                        for(auto& m : resp.messages()) {
                            out += "\n [" + std::to_string(m.created_at()) + "] " + m.from_username() + ": "+ m.content();
                        }
                        msg_callback_(out);
                    }
                }
                break;
            }
            case p::MSG_OFFLINE_NOTIFY:
            {
                p::OfflineMessageNotify notify;
                if (notify.ParseFromArray(body.data(), body.size())) {
                    if (msg_callback_) {
                        std::string msg = "[离线消息] 收到 " + std::to_string(notify.total_count()) + " 条离线消息";
                        if (notify.private_count() > 0) {
                            msg += " (私聊: " + std::to_string(notify.private_count()) + "条)";
                        }
                        if (notify.group_count() > 0) {
                            msg += " (群聊: " + std::to_string(notify.group_count()) + "条)";
                        }
                        msg_callback_(msg);
                    }
                    // 请求离线消息内容
                    getOfflineMessages();
                } else {
                    LOG_ERROR << "Failed to parse OfflineMessageNotify";
                }
                break;
            }
            case p::MSG_FILE_MESSAGE:
            {
                db::FileMessage file_msg;
                if(file_msg.ParseFromArray(body.data(), body.size())) {
                    if(msg_callback_) {
                        msg_callback_("[文件] " + std::to_string(file_msg.from_uid()) + " 发送：" + file_msg.filename() + " (ID:" + file_msg.file_id() + ")");
                    }
                }
                break;
            }
            case p::MSG_CONVERSATION_LIST:
            {
                p::ConversationListResponse resp;
                if(resp.ParseFromArray(body.data(), body.size())) {
                    handleConversationListResponse(resp);
                }
                break;
            }
            case p::MSG_BLOCK_LIST:
            {
                p::BlockListResponse resp;
                if(resp.ParseFromArray(body.data(), body.size())) {
                    handleBlockListResponse(resp);
                }
                break;
            }
            case p::MSG_FILE_CHUNK:
            {
                db::FileChunk chunk;
                if(chunk.ParseFromArray(body.data(), body.size())) {
                    if(msg_callback_) {
                        msg_callback_("[文件] 接收: " + chunk.file_id() + " #" + std::to_string(chunk.chunk_index()));
                    }

                    saveChunkToFile(chunk);
                }
                break;
            }
            case p::MSG_FILE_DOWNLOAD_REQ:
            {
                db::FileDownloadReq req;
                if(req.ParseFromArray(body.data(), body.size())) {
                    if(msg_callback_) {
                        msg_callback_("[文件] 开始下载：" + req.file_id());
                    }
                }
                break;
            }
            case p::MSG_DELETE_ACCOUNT:
            {
                p::CommonResponse resp;
                if(resp.ParseFromArray(body.data(), body.size())) {
                    handleDeleteAccountResponse(resp);
                }
                break;
            }
            case p::MSG_FILE_UPLOAD_RESP:
            {
                db::FileUploadResp resp;
                if (resp.ParseFromArray(body.data(), body.size())) {
                    handleFileUploadResp(resp);
                }
                break;
            }
            case p::MSG_FILE_UPLOAD_COMPLETE:
            {
                db::FileUploadComplete complete;
                if (complete.ParseFromArray(body.data(), body.size())) {
                    handleFileUploadComplete(complete);
                }
                break;
            }
            case p::MSG_FILE_DOWNLOAD_RESP:
            {
                db::FileDownloadResp resp;
                if (resp.ParseFromArray(body.data(), body.size())) {
                    handleFileDownloadResp(resp);
                }
                break;
            }
            case p::MSG_FILE_DOWNLOAD_CHUNK:
            {
                db::FileDownloadChunk chunk;
                if (chunk.ParseFromArray(body.data(), body.size())) {
                    saveDownloadChunk(chunk);
                }
                break;
            }
            case p::MSG_FILE_RESUME_RESP:
            {
                db::FileResumeResp resp;
                if (resp.ParseFromArray(body.data(), body.size())) {
                    handleFileResumeResp(resp);
                }
                break;
            }
            case p::MSG_GROUP_FILE_MESSAGE:
            {
                db::FileMessage file_msg;
                if (file_msg.ParseFromArray(body.data(), body.size())) {
                    if (msg_callback_) {
                        msg_callback_("[群组文件] " + std::to_string(file_msg.from_uid()) +
                                    " 发送了: " + file_msg.filename() +
                                    " (ID: " + file_msg.file_id() + ")");
                    }
                }
                break;
            }
            case p::MSG_FILE_OFFLINE_DOWNLOAD:
            {
                db::FileOfflineListResp resp;
                if (resp.ParseFromArray(body.data(), body.size())) {
                    handleOfflineFileListResp(resp);
                }
                break;
            }
            case p::MSG_COMMON_RESPONSE:
            {
                p::CommonResponse resp;
                if (resp.ParseFromArray(body.data(), body.size())) {
                    if (msg_callback_) {
                        std::string prefix = (resp.code() == 0) ? "[成功]" : "[失败]";
                        msg_callback_(prefix + " " + resp.message());
                    }
                }
                break;
            }
            case p::MSG_COMMON_REQUEST:
            {
                p::CommonResponse resp;
                if (resp.ParseFromArray(body.data(), body.size())) {
                    if (msg_callback_) {
                        std::string msg = "[通知] " + resp.message();
                        // 如果是好友请求通知，可以特殊处理
                        if (header.request_id() > 0) {
                            msg += " (请求ID: " + std::to_string(header.request_id()) + ")";
                            msg += "，使用 /friendprocess " + std::to_string(header.request_id()) + 
                                " true/false 处理";
                        }
                        msg_callback_(msg);
                    }
                    if(resp.code() == 0 && resp.message().find("recalled") != std::string::npos) {
                        getConversationList();
                    }
                }
                break;
            }
            case p::MSG_ADD_FRIEND:
            {
                p::CommonResponse resp;
                if (resp.ParseFromArray(body.data(), body.size())) {
                    if (msg_callback_) {
                        std::string prefix = (resp.code() == 0) ? "[成功]" : "[失败]";
                        msg_callback_(prefix + " " + resp.message());
                    }
                }
                break;
            }
            case p::MSG_DELETE_FRIEND:
            {
                p::CommonResponse resp;
                if (resp.ParseFromArray(body.data(), body.size())) {
                    if (msg_callback_) {
                        std::string prefix = (resp.code() == 0) ? "[成功]" : "[失败]";
                        msg_callback_(prefix + " 删除好友: " + resp.message());
                    }
                }
                break;
            }
            case p::MSG_PROCESS_FRIEND_REQUEST:
            {
                p::CommonResponse resp;
                if (resp.ParseFromArray(body.data(), body.size())) {
                    if (msg_callback_) {
                        std::string prefix = (resp.code() == 0) ? "[成功]" : "[失败]";
                        msg_callback_(prefix + " " + resp.message());
                    }
                    // 成功后刷新好友列表
                    if (resp.code() == 0) {
                        getFriendList();
                    }
                }
                break;
            }
            case p::MSG_RECALL:
            {
                p::MessageRecall recall;
                if (recall.ParseFromArray(body.data(), body.size())) {
                    if (msg_callback_) {
                        uint64_t from = recall.from_uid();
                        uint64_t msg_id = recall.msg_id();
                        
                        std::string msg = "[撤回] 用户 " + std::to_string(from) + 
                                        " 撤回了消息 (ID:" + std::to_string(msg_id) + ")";
                        
                        msg_callback_(msg);
                    }
                    // 刷新会话列表
                    getConversationList();
                }
                break;
            }
            default:
                LOG_DEBUG << "Unhandle message type: " << header.msg_type();
                break; 
        }
    }

    void setMessageCallback(MessageCallback cb) {
        msg_callback_ = cb;
    }

    bool sendData(const std::vector<char>& data) {
        if(!client_->isConnected()) {
            return false;
        }
        LOG_DEBUG << "sending" << data.size();
        client_->send(data.data(), data.size());
        return true;
    }

    // void sendHeartbeat() {
    //     if(!user_.logged_in) {
    //         return ;
    //     }
    //     p::Heartbeat hb;
    //     hb.set_timestamp(tool::getTimestamp());
    //     hb.set_client_time(tool::getTimestamp());
    //     hb.set_seq(++heartbeat_seq_);

    //     p::MessageHeader header;
    //     header.set_msg_type(p::MSG_HEARTBEAT);
    //     header.set_timestamp(tool::getTimestamp());

    //     auto data = proto::MessageCodec::encode(header, hb);
    //     if(!data.empty()) {
    //         sendData(data);
    //     }
    // }

    // void startHeartbeat(int time) {
    //     if(heartbeat_running_) {
    //         return ;
    //     }
    //     heartbeat_interval_ = time;
    //     heartbeat_running_ = true;
    //     heartbeat_thread_ = std::thread([this]() {
    //         std::unique_lock<std::mutex> lock(heartbeat_mutex_);
    //         while(heartbeat_running_) {
    //             // 可被 stopHeartbeat 唤醒，避免关闭时 join 阻塞 heartbeat_interval 秒
    //             heartbeat_cv_.wait_for(lock, std::chrono::seconds(heartbeat_interval_), [this] {
    //                 return !heartbeat_running_;
    //             });
    //             if(!heartbeat_running_) {
    //                 break;
    //             }
    //             lock.unlock();
    //             if(user_.logged_in) {
    //                 sendHeartbeat();
    //             }
    //             lock.lock();
    //         }
    //     });
    // }

    // void stopHeartbeat() {
    //     {
    //         std::lock_guard<std::mutex> lock(heartbeat_mutex_);
    //         heartbeat_running_ = false;
    //     }
    //     heartbeat_cv_.notify_all();
    //     if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    // }

    void Login(const std::string& username, const std::string& password, const std::string& device_id) {
        // 连接已断开时自动重连，避免登录请求发送到已关闭的连接导致超时
        if(!client_->isConnected()) {
            if(!reconnect()) {
                if(msg_callback_) {
                    msg_callback_("[错误] 无法连接到服务器");
                }
                return;
            }
            user_.logged_in = false;
            user_.uid = 0;
        }

        p::LoginRequest req;
        req.set_username(username);
        req.set_password(password);
        req.set_device_id(device_id);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_LOGIN);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            // 先置位再发送，避免响应在置位前到达导致丢失唤醒
            {
                std::lock_guard<std::mutex> lock(login_mutex_);
                login_response_received_ = false;
            }
            sendData(data);
        }

        std::unique_lock<std::mutex> lock(login_mutex_);
        login_cv_.wait_for(lock, std::chrono::seconds(5), [this] {return login_response_received_;});
        if(!login_response_received_) {
            if(msg_callback_) {
                msg_callback_("[错误] 登录超时");
            }
        }
    }

    void registerUser(const std::string& username, const std::string& password, const std::string& email, const std::string& nickname) {
        LOG_DEBUG << "registeruser called";
        if(!client_->isConnected()) {
            if(!reconnect()) {
                if(msg_callback_) {
                    msg_callback_("[错误] 无法连接到服务器");
                }
                return;
            }
        }

        p::RegisterRequest req;
        req.set_username(username);
        req.set_password(password);
        req.set_email(email);
        req.set_nickname(nickname.empty() ? username : nickname);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_REGISTER);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void deleteUser(const std::string& password) {
        if(!user_.logged_in) {
            LOG_ERROR << "[错误] 未登录";
            return ;
        }

        p::DeleteAccountRequest req;
        req.set_token(user_.tID);
        if(!password.empty()) {
            req.set_password(password);
        }
        req.set_confirm(true);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_DELETE_ACCOUNT);
        header.set_timestamp(tool::getTimestamp());
        header.set_from_uid(user_.uid);

        auto data = proto::MessageCodec::encode(header, req);
        if(data.empty()) {
            if(msg_callback_) {
                msg_callback_("[错误] 失败");
                return ;
            }
        }

        // 先置位再发送，避免响应在置位前到达导致丢失唤醒
        {
            std::lock_guard<std::mutex> lock(delete_mutex_);
            delete_response_received_ = false;
            delete_success_ = false;
        }

        if(!sendData(data)) {
            if(msg_callback_) {
                msg_callback_("[错误] 发送失败");
            }
            return ;
        }

        {
            std::unique_lock<std::mutex> lock(delete_mutex_);
            bool result = delete_cv_.wait_for(lock, std::chrono::seconds(30),[this]() -> bool { return delete_response_received_; });
            
            if(!result) {
                if(msg_callback_) {
                    msg_callback_("[错误] 删除超时");
                }
                return ;
            }
        }
        if(delete_success_) {
            LOG_INFO << "deleteUser: success, showing confirmation";
            if(msg_callback_) {
                msg_callback_("[系统] 账户删除成功");
            }

            user_.logged_in = false;
            user_.uid = 0;
            user_.username.clear();
            user_.nickname.clear();
            user_.tID.clear();

            disconnect();

            if(ui_callback_) {
                ui_callback_("logout");
            }
        }
        else {
            if(msg_callback_) {
                msg_callback_("[错误] 删除账户失败");
            }
        }
    
    }

    void Logout() {
        p::LogoutRequest req;
        req.set_token(user_.tID);
        
        p::MessageHeader header;
        header.set_msg_type(p::MSG_LOGOUT);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
        user_.logged_in = false;
        //stopHeartbeat();
    }
    
    void sendprivateChat(uint64_t to_uid, const std::string& content, int msg_type = 1) {
        p::ChatMessage msg;
        msg.set_from_uid(user_.uid);
        msg.set_to_uid(to_uid);
        msg.set_content(content);
        msg.set_msg_type(msg_type);

        p::MessageHeader header;
        header.set_from_uid(user_.uid);
        header.set_to_uid(to_uid);
        header.set_msg_type(p::MSG_CHAT);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, msg);
        if(!data.empty()) {
            sendData(data);
            if(msg_callback_) {
                std::string to_name = std::to_string(to_uid);
                for(const auto& conv : conversation_list_) {
                    if(conv.user_id == to_uid) {
                        to_name = conv.nickname;
                        break;
                    }
                }
            }
        }
    }

    void sendGroupChat(uint64_t group_id, const std::string& content, int msg_type = 1) {
        p::GroupChatRequest req;
        req.set_msg_type(msg_type);
        req.set_group_id(group_id);
        req.set_content(content);
        
        p::MessageHeader header;
        header.set_msg_type(p::MSG_GROUP_CHAT);
        header.set_from_uid(user_.uid);
        header.set_to_uid(group_id);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void getHistory(uint64_t target_uid, int limit, int64_t before_time = 0) {
        p::GetHistoryRequest req;
        req.set_target_uid(target_uid);
        req.set_limit(limit);
        req.set_before_time(before_time);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_HISTORY);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void getGroupHistory(uint64_t group_id, int limit, int64_t before_time = 0) {
        p::GroupHistoryRequest req;
        req.set_group_id(group_id);
        req.set_limit(limit);
        req.set_before_time(before_time);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_GROUP_HISTORY);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void markRead(uint64_t from_uid) {
        p::MarkReadRequest req;
        req.set_from_uid(from_uid);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_READ_RECEIPT);
        header.set_timestamp(tool::getTimestamp());
        
        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void markGroupRead(uint64_t group_id) {
        p::MarkGroupReadRequest req;
        req.set_group_id(group_id);
        req.set_last_msg_id(0);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_GROUP_READ);
        header.set_timestamp(tool::getTimestamp());
        
        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void recallMessage(uint64_t msg_id) {
        p::RecallMessageRequest req;
        req.set_msg_id(msg_id);
        
        p::MessageHeader header;
        header.set_msg_type(p::MSG_RECALL);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void recallGroupMessage(uint64_t msg_id) {
        p::RecallGroupMessageRequest req;
        req.set_msg_id(msg_id);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_GROUP_RECALL);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void getConversationList() {
        if(!user_.logged_in) {
            if(msg_callback_) {
                msg_callback_("[错误] 未登录");
            }
            return ;
        }
        p::ConversationListRequest req;

        p::MessageHeader header;
        header.set_msg_type(p::MSG_CONVERSATION_LIST);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
            LOG_DEBUG << "Request conversation list";
        }
    }

    void getGroupList() {
        if(!user_.logged_in) {
            if(msg_callback_) {
                msg_callback_("[错误] 未登录，无法获取群组列表");
            }
            LOG_DEBUG << "Not logged in, cannot get group list";
            return;
        }
        
        LOG_DEBUG << "Requesting group list for user " << user_.uid;
        
        p::GetGroupListRequest req;
        p::MessageHeader header;
        header.set_msg_type(p::MSG_GROUP_LIST);
        header.set_timestamp(tool::getTimestamp());
        header.set_from_uid(user_.uid);
        header.set_to_uid(user_.uid);

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
            LOG_DEBUG << "Group list request sent";
        } else {
            LOG_ERROR << "Failed to encode group list request";
        }
    }

    void getGroupMembers(uint64_t group_id) {
        p::GetGroupMembersRequest req;
        req.set_group_id(group_id);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_GROUP_MEMBERS);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void creatgroup(const std::string& name, const std::string& desc, bool is_public, int  join_type, const std::vector<uint64_t>& initial_members = {}, int max_members = 500) {
        p::CreateGroupRequest req;
        req.set_group_name(name);
        req.set_description(desc);
        req.set_is_public(is_public);
        req.set_join_type(join_type);
        req.set_max_members(max_members);

        for(uint64_t uid : initial_members) {
            req.add_initial_member_ids(uid);
        }

        p::MessageHeader header;
        header.set_msg_type(p::MSG_GROUP_CREATE);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void joinGroup(uint64_t group_id, const std::string& message) {
        p::JoinGroupRequest req;
        req.set_group_id(group_id);
        req.set_message(message);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_GROUP_JOIN);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void leaveGroup(uint64_t group_id) {
        p::LeaveGroupRequest req;
        req.set_group_id(group_id);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_GROUP_LEAVE);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void dismissGroup(uint64_t group_id) {
        p::DismissGroupRequest req;
        req.set_group_id(group_id);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_GROUP_DISMISS);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void processJoinRequest(uint64_t request_id, bool accept) {
        p::ProcessJoinRequest req;
        req.set_request_id(request_id);
        req.set_accept(accept);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_PROCESS_REQUEST);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void getPendingRequests(uint64_t group_id) {
        p::GetPendingRequestsRequest req;
        req.set_group_id(group_id);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_PENDING_REQUESTS);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void setAdmin(uint64_t group_id, uint64_t target_uid, bool is_admin) {
        p::SetAdminRequest req;
        req.set_group_id(group_id);
        req.set_target_uid(target_uid);
        req.set_is_admin(is_admin);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_SET_ADMIN);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void kickMember(uint64_t group_id, uint64_t target_uid) {
        p::KickMemberRequest req;
        req.set_group_id(group_id);
        req.set_target_uid(target_uid);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_KICK_MEMBER);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void getFriendList() {
        if(!user_.logged_in) {
            if(msg_callback_) {
                msg_callback_("[错误] 未登录");
            }
            return ;
        }

        p::GetFriendListRequest req;

        p::MessageHeader header;
        header.set_msg_type(p::MSG_FRIEND_LIST);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
            LOG_DEBUG << "Request friend list";
        }
    }

    void sendFriendRequest(uint64_t to_uid, const std::string& message) {
        p::FriendRequest req;
        req.set_from_uid(user_.uid);
        req.set_to_uid(to_uid);
        req.set_message(message);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_ADD_FRIEND);
        header.set_timestamp(tool::getTimestamp());
        header.set_from_uid(user_.uid);
        
        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void processFriendRequest(uint64_t request_id, bool accept) {
        p::ProcessFriendRequest req;
        req.set_request_id(request_id);
        req.set_accept(accept);
        
        p::MessageHeader header;
        header.set_msg_type(p::MSG_PROCESS_FRIEND_REQUEST);
        header.set_timestamp(tool::getTimestamp());
        
        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void deleteFriend(uint64_t friend_id) {
        p::DeleteFriendRequest req;
        req.set_friend_id(friend_id);
        
        p::MessageHeader header;
        header.set_msg_type(p::MSG_DELETE_FRIEND);
        header.set_timestamp(tool::getTimestamp());
        
        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void blockUser(uint64_t uid) {
        p::BlockUserRequest req;
        req.set_block_id(uid);
        
        p::MessageHeader header;
        header.set_msg_type(p::MSG_BLOCK_USER);
        header.set_timestamp(tool::getTimestamp());
        header.set_from_uid(user_.uid);
        
        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void unblockUser(uint64_t uid) {
        p::UnblockUserRequest req;
        req.set_block_id(uid);
        p::MessageHeader header;
        header.set_msg_type(p::MSG_UNBLOCK_USER);
        header.set_timestamp(tool::getTimestamp());
        header.set_from_uid(user_.uid);

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void getBlockList() {
        if(!user_.logged_in) {
            if(msg_callback_) {
                msg_callback_("[错误] 未登录");
            }
            return ;
        }

        p::BlockListRequest req;
        p::MessageHeader header;
        header.set_msg_type(p::MSG_BLOCK_LIST);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    void handleBlockListResponse(const p::BlockListResponse& resp) {
        block_list_.clear();

        for(int i = 0; i < resp.block_ids_size(); ++i) {
            block_list_.push_back(resp.block_ids(i));
        }

        if(msg_callback_) {
            if(block_list_.empty()) {
                msg_callback_("[系统] 屏蔽列表为空");
            }
            else {
                std::string out = "屏蔽列表（" + std::to_string(block_list_.size()) + "人):";
                for(uint64_t uid : block_list_) {
                    out += "\n 用户ID：" + std::to_string(uid);
                }
                msg_callback_(out);
            }
        }
        LOG_DEBUG << "Received block list, size: " + block_list_.size();
    }

    bool getResumeOffset(const std::string& file_id, int direction, uint64_t& offset) {
        uint64_t req_id = next_request_id_.fetch_add(1);
        auto promise = std::make_shared<std::promise<ResumeResult>>();
        auto future = promise->get_future();

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_[req_id] = promise;
        }

        db::FileResumeReq req;
        req.set_file_id(file_id);
        req.set_direction(direction);
        req.set_request_id(req_id);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_FILE_RESUME_REQ);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(data.empty()) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_.erase(req_id);
            LOG_ERROR << "Encode FileResumeReq failed";
            return false;
        }

        if(!sendData(data)) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_.erase(req_id);
            LOG_ERROR << "Send FileResumeReq failed";
            return false;
        }

        auto status = future.wait_for(std::chrono::seconds(5));
        if(status == std::future_status::timeout) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_.erase(req_id);
            LOG_ERROR << "FileResumeResp timeout for req " << req_id;
            return false;
        }

        ResumeResult result = future.get();
        if(!result.success) {
            LOG_ERROR << "Server returned error for req " << req_id;
            return false;
        }

        offset = result.offset;
        LOG_INFO << "Resume offset: " << offset << " for file " << file_id;
        return true;
    }

    void sendFile(uint64_t to_uid, const std::string& file_path, const std::string& text) {
        std::ifstream file(file_path, std::ios::binary | std::ios::ate);
        if(!file) {
            if (msg_callback_) msg_callback_("[错误] 无法打开文件: " + file_path);
            return;
        }
        uint64_t file_size = file.tellg();
        file.seekg(0);
        std::string filename = file_path.substr(file_path.find_last_of("/\\") + 1);
        std::string md5 = MD5Tool::calculateFile(file_path);

        std::string file_id = md5 + "_" + std::to_string(user_.uid);

        // 查询断点偏移
        uint64_t start_offset = 0;
        if(getResumeOffset(file_id, 0, start_offset)) {
            if(start_offset >= file_size) {
                if (msg_callback_) msg_callback_("[文件] 文件已完整上传，跳过");
                return;
            }
            if(msg_callback_)
                msg_callback_("[文件] 继续上传，从偏移 " + std::to_string(start_offset));
        }
        else {
            start_offset = 0;  // 查询失败从头开始
        }

        uint64_t req_id = next_request_id_.fetch_add(1);
        auto promise = std::make_shared<std::promise<UploadMetaResult>>();
        auto future = promise->get_future();

        {
            std::lock_guard<std::mutex> lock(upload_meta_mutex_);
            pending_meta_requests_[req_id] = promise;
        }

        db::FileInfo info;
        info.set_file_id(file_id);
        info.set_filename(filename);
        info.set_file_size(file_size);
        info.set_md5(md5);
        info.set_to_uid(to_uid);
        info.set_from_uid(user_.uid);
        info.set_upload_time(tool::getTimestamp());
        info.set_expire_time(info.upload_time() + 7 * 24 * 3600 * 1000);
        info.set_status(0);

        db::FileUploadReq req;
        req.mutable_file_info()->CopyFrom(info);
        req.set_chunk_size(64 * 1024);
        req.set_request_id(req_id);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_FILE_UPLOAD_REQ);
        header.set_timestamp(tool::getTimestamp());
        auto data = proto::MessageCodec::encode(header, req);
        if(data.empty() || !sendData(data)) {
            std::lock_guard<std::mutex> lock(upload_meta_mutex_);
            pending_meta_requests_.erase(req_id);
            if(msg_callback_)
                msg_callback_("[错误] 发送文件元数据失败");
            return;
        }

        auto status = future.wait_for(std::chrono::seconds(10));
        if(status == std::future_status::timeout) {
            std::lock_guard<std::mutex> lock(upload_meta_mutex_);
            pending_meta_requests_.erase(req_id);
            if(msg_callback_)
                msg_callback_("[错误] 获取上传凭证超时");
            return;
        }
        UploadMetaResult meta = future.get();
        if(!meta.success) {
            if(msg_callback_) msg_callback_("[错误] 服务端拒绝上传: " + meta.error_msg);
            return;
        }
        if(meta.is_duplicate) {
            if(msg_callback_) msg_callback_("[文件] 已有相同文件，ID: " + meta.existing_file_id);
            return;
        }
        // 若服务端分配了新的file_id，则使用它
        if(!meta.file_id.empty())
            file_id = meta.file_id;
        if(meta.resume_offset > start_offset)
            start_offset = meta.resume_offset;

        // 分块发送
        sendFileChunks(file_id, file_path, start_offset);
    }

    void sendGroupFile(uint64_t group_id, const std::string& file_path, const std::string& text) {
        // 检查登录状态
        if(!user_.logged_in) {
            if (msg_callback_) msg_callback_("[错误] 未登录");
            return;
        }

        // 打开文件
        std::ifstream file(file_path, std::ios::binary | std::ios::ate);
        if(!file) {
            if (msg_callback_) msg_callback_("[错误] 无法打开文件: " + file_path);
            return;
        }
        uint64_t file_size = file.tellg();
        file.seekg(0);
        std::string filename = file_path.substr(file_path.find_last_of("/\\") + 1);
        std::string md5 = MD5Tool::calculateFile(file_path);

        // 生成文件ID
        std::string file_id = md5 + "_" + std::to_string(user_.uid);

        // 查询断点偏移
        uint64_t start_offset = 0;
        if(getResumeOffset(file_id, 2, start_offset)) {
            if(start_offset >= file_size) {
                if(msg_callback_)
                    msg_callback_("[文件] 文件已完整上传，跳过");
                return;
            }
            if(msg_callback_)
                msg_callback_("[文件] 继续上传群组文件，从偏移 " + std::to_string(start_offset));
        }
        else {
            start_offset = 0;
        }

        // 发送元数据
        uint64_t req_id = next_request_id_.fetch_add(1);
        auto promise = std::make_shared<std::promise<UploadMetaResult>>();
        auto future = promise->get_future();

        {
            std::lock_guard<std::mutex> lock(upload_meta_mutex_);
            pending_meta_requests_[req_id] = promise;
        }

        db::FileInfo info;
        info.set_file_id(file_id);
        info.set_filename(filename);
        info.set_file_size(file_size);
        info.set_md5(md5);
        info.set_group_id(group_id);
        info.set_from_uid(user_.uid);
        info.set_upload_time(tool::getTimestamp());
        info.set_expire_time(info.upload_time() + 7 * 24 * 3600 * 1000);
        info.set_status(0);

        // 构造群组上传请求通过file_info.group_id识别群聊
        db::FileUploadReq req;
        req.mutable_file_info()->CopyFrom(info);
        req.set_chunk_size(64 * 1024);
        req.set_request_id(req_id);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_FILE_UPLOAD_REQ);
        header.set_timestamp(tool::getTimestamp());
        auto data = proto::MessageCodec::encode(header, req);
        if(data.empty() || !sendData(data)) {
            std::lock_guard<std::mutex> lock(upload_meta_mutex_);
            pending_meta_requests_.erase(req_id);
            if(msg_callback_) 
                msg_callback_("[错误] 发送群组文件元数据失败");
            return;
        }

        // 等待元数据响应
        auto status = future.wait_for(std::chrono::seconds(10));
        if(status == std::future_status::timeout) {
            std::lock_guard<std::mutex> lock(upload_meta_mutex_);
            pending_meta_requests_.erase(req_id);
            if (msg_callback_) msg_callback_("[错误] 获取上传凭证超时");
            return;
        }
        UploadMetaResult meta = future.get();
        if(!meta.success) {
            if (msg_callback_) msg_callback_("[错误] 服务端拒绝上传: " + meta.error_msg);
            return;
        }
        if(meta.is_duplicate) {
            if(msg_callback_) msg_callback_("[文件] 已有相同文件，ID: " + meta.existing_file_id);
            return;
        }
        if(!meta.file_id.empty())
            file_id = meta.file_id;
        if(meta.resume_offset > start_offset)
            start_offset = meta.resume_offset;

        // 分块发送
        sendGroupFileChunks(file_id, group_id, file_path, start_offset);
    }

    // 从file_id中提取MD5
    static std::string md5FromFileId(const std::string& file_id) {
        if(file_id.size() >= 32) {
            std::string prefix = file_id.substr(0, 32);
            bool all_hex = true;
            for(char c : prefix) {
                if(!std::isxdigit(static_cast<unsigned char>(c))) {
                    all_hex = false;
                    break;
                }
            }
            if(all_hex) return prefix;
        }
        return "";
    }

    static std::string normalizeFileId(const std::string& id) {
        std::string s = id;
        size_t begin = s.find_first_not_of(" \t\r\n");
        if(begin == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        s = s.substr(begin, end - begin + 1);
        if(s.size() > 3 && (s[0] == 'I' || s[0] == 'i') && (s[1] == 'D' || s[1] == 'd') && s[2] == ':') {
            s = s.substr(3);
            size_t b2 = s.find_first_not_of(" \t\r\n");
            if(b2 == std::string::npos) return "";
            s = s.substr(b2);
        }
        return s;
    }

    // 在本地目录查找是否已下载过相同内容的文件，返回其file_id
    static std::string findLocalFileByMD5(const std::string& md5) {
        if(md5.empty()) return "";
        std::error_code ec;
        for(const auto& entry : std::filesystem::directory_iterator(".", ec)) {
            if(ec) break;
            if(!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();
            if(name.size() > 4 &&
               name.compare(name.size() - 4, 4, ".dat") == 0 &&
               name.compare(0, md5.size(), md5) == 0) {
                return name.substr(0, name.size() - 4);
            }
        }
        return "";
    }

    void downloadFile(const std::string& raw_file_id) {
        std::string file_id = normalizeFileId(raw_file_id);

        // 下载去重：若本地磁盘已存在相同内容的文件，则不再重复下载
        std::string md5 = md5FromFileId(file_id);
        if(!md5.empty()) {
            std::string existing = findLocalFileByMD5(md5);
            if(!existing.empty()) {
                if(msg_callback_)
                    msg_callback_("[文件] 已有相同文件，ID: " + existing);
                return;
            }
        }

        uint64_t start_offset = 0;
        if(!getResumeOffset(file_id, 1, start_offset)) {
            start_offset = 0;
        }

        db::FileDownloadReq req;
        req.set_file_id(file_id);
        req.set_offset(start_offset);

        p::MessageHeader header;
        header.set_msg_type(p::MSG_FILE_DOWNLOAD_REQ);
        header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(header, req);
        if(data.empty() || !sendData(data)) {
            if(msg_callback_)
                msg_callback_("[错误] 发送下载请求失败");
            return;
        }

        if(msg_callback_) 
            msg_callback_("[文件] 开始下载，文件ID: " + file_id);
    }

    void sendFileChunks(const std::string& file_id, const std::string& file_path, uint64_t start_offset) {
        const uint32_t CHUNK_SIZE = 64 * 1024;
        //int chunk_count = 0;
        std::ifstream file(file_path, std::ios::binary);
        if(!file) {
            if(msg_callback_) msg_callback_("[错误] 无法打开文件: " + file_path);
            return;
        }
        file.seekg(start_offset);
        if(!file) {
            if(msg_callback_) msg_callback_("[错误] 文件定位到偏移 " + std::to_string(start_offset) + " 失败");
            return;
        }

        // 计算起始chunk_index，使服务端能连续性识别
        uint32_t chunk_index = static_cast<uint32_t>(start_offset / CHUNK_SIZE);
        uint64_t current_offset = start_offset;
        std::vector<char> buffer(CHUNK_SIZE);

        while(file) {
            file.read(buffer.data(), CHUNK_SIZE);
            std::streamsize bytes_read = file.gcount();
            if(bytes_read <= 0) break;

            // 通过peek判断最后一块，正确处理文件大小为块大小整数倍的情况
            bool is_last = (file.peek() == EOF);

            db::FileUploadChunk chunk;
            chunk.set_file_id(file_id);
            chunk.set_chunk_index(chunk_index);
            chunk.set_offset(current_offset);
            chunk.set_data(buffer.data(), bytes_read);
            chunk.set_is_last(is_last);

            p::MessageHeader header;
            header.set_msg_type(p::MSG_FILE_UPLOAD_CHUNK);
            header.set_timestamp(tool::getTimestamp());

            auto data = proto::MessageCodec::encode(header, chunk);
            if(data.empty() || !sendData(data)) {
                if(msg_callback_)
                    msg_callback_("[错误] 分片上传失败，偏移 " + std::to_string(current_offset));
                return;
            }

            current_offset += bytes_read;
            chunk_index++;

            // chunk_count++;
            // if(chunk_count % 20 == 0) {
            //     sendHeartbeat();
            // }

            // 轻微限速，免瞬间塞满服务端接收缓冲
        }

        // 全部发送完成
        LOG_INFO << "File upload finished: " << file_id
                << ", total sent " << (current_offset - start_offset) << " bytes";
        if(msg_callback_) {
            msg_callback_("[文件] 上传完成，共 " + std::to_string(current_offset - start_offset) + " 字节");
        }
    }

    void sendGroupFileChunks(const std::string& file_id, uint64_t group_id, const std::string& file_path, uint64_t start_offset) {
        const uint32_t CHUNK_SIZE = 64 * 1024;

        std::ifstream file(file_path, std::ios::binary);
        if(!file) {
            if (msg_callback_) msg_callback_("[错误] 无法打开文件: " + file_path);
            return;
        }
        file.seekg(start_offset);
        if(!file) {
            if (msg_callback_) msg_callback_("[错误] 文件定位失败");
            return;
        }

        uint32_t chunk_index = static_cast<uint32_t>(start_offset / CHUNK_SIZE);
        uint64_t current_offset = start_offset;
        std::vector<char> buffer(CHUNK_SIZE);
        //int chunk_count = 0;
        
        while(file) {
            file.read(buffer.data(), CHUNK_SIZE);
            std::streamsize bytes_read = file.gcount();
            if (bytes_read <= 0) break;

            bool is_last = (file.peek() == EOF);

            db::FileUploadChunk chunk;
            chunk.set_file_id(file_id);
            chunk.set_chunk_index(chunk_index);
            chunk.set_offset(current_offset);
            chunk.set_data(buffer.data(), bytes_read);
            chunk.set_is_last(is_last);

            p::MessageHeader header;
            header.set_msg_type(p::MSG_FILE_UPLOAD_CHUNK);
            header.set_timestamp(tool::getTimestamp());

            auto data = proto::MessageCodec::encode(header, chunk);
            if(data.empty() || !sendData(data)) {
                if (msg_callback_)
                    msg_callback_("[错误] 群组分片上传失败，偏移 " + std::to_string(current_offset));
                return;
            }

            current_offset += bytes_read;
            chunk_index++;

            // chunk_count++;
            // if (chunk_count % 20 == 0) {
            //     sendHeartbeat();  // 主动心跳
            // }

        }

        LOG_INFO << "Group file upload finished: " << file_id;
        if (msg_callback_) msg_callback_("[文件] 群组文件上传完成");
    }

    void saveChunkToFile(const db::FileChunk& chunk) {
        const std::string& file_id = chunk.file_id();
        uint64_t offset = chunk.offset();
        const std::string& data = chunk.data();
        bool is_last = chunk.is_last();

        std::lock_guard<std::mutex> lock(file_mutex_);

        // 创建文件流
        auto it = downloading_files_.find(file_id);
        if(it == downloading_files_.end()) {
            std::string temp_filename = file_id + ".tmp";
            std::ofstream fs;
            if (offset > 0) {
                // 断点续传：保留已写入的内容，避免 trunc 破坏前半部分
                fs.open(temp_filename, std::ios::binary | std::ios::in | std::ios::out);
            }
            if (!fs.is_open()) {
                // 从头下载或文件不存在：新建（存在则截断）
                fs.open(temp_filename, std::ios::binary | std::ios::out | std::ios::trunc);
            }
            if(!fs) {
                if (msg_callback_) msg_callback_("[错误] 无法创建文件: " + temp_filename);
                return;
            }
            // 将对象存入map
            downloading_files_.emplace(file_id, std::move(fs));
            // 指向刚插入的元素
            it = downloading_files_.find(file_id);
        }

        std::ofstream& fs = it->second;

        // 定位到指定偏移量写入
        fs.seekp(offset);
        if(!fs) {
            if(msg_callback_) 
                msg_callback_("[错误] 文件定位失败 (offset=" + std::to_string(offset) + ")");
            fs.close();
            downloading_files_.erase(it);
            return;
        }

        // 写入数据
        fs.write(data.data(), data.size());
        if(!fs) {
            if (msg_callback_) msg_callback_("[错误] 写入文件失败 (file_id=" + file_id + ")");
            fs.close();
            downloading_files_.erase(it);
            return;
        }

        // 如果是最后一个块，关闭文件并重命名
        if (is_last) {
            fs.close();
            std::string temp_name = file_id + ".tmp";
            std::string final_name;
            auto map_it = download_filename_map_.find(file_id);
            if (map_it != download_filename_map_.end() && !map_it->second.empty()) {
                final_name = map_it->second;
            }
            else {
                final_name = file_id + ".dat";
            }
            // 删除已存在的同名文件
            if(remove(final_name.c_str()) != 0 && errno != ENOENT) {
                if(msg_callback_) msg_callback_("[错误] 删除旧文件失败: " + final_name);
            }
            if(rename(temp_name.c_str(), final_name.c_str()) != 0) {
                if (msg_callback_) msg_callback_("[错误] 重命名文件失败: " + temp_name + " -> " + final_name);
            }
            else {
                if (msg_callback_) msg_callback_("[文件] 下载完成: " + final_name);
            }
            download_filename_map_.erase(file_id);
            downloading_files_.erase(it);
        }
    }

    void getOfflineFiles() {
        db::FileOfflineListReq req;
        req.set_limit(100);
        p::MessageHeader header;
        header.set_msg_type(p::MSG_FILE_OFFLINE_DOWNLOAD);
        header.set_timestamp(tool::getTimestamp());
        auto data = proto::MessageCodec::encode(header, req);
        if(!data.empty()) {
            sendData(data);
        }
    }

    // 文件上传响应
    void handleFileUploadResp(const db::FileUploadResp& resp) {
        // 解析等待中的元数据请求
        std::shared_ptr<std::promise<UploadMetaResult>> promise;
        {
            std::lock_guard<std::mutex> lock(upload_meta_mutex_);
            if(!pending_meta_requests_.empty()) {
                auto it = pending_meta_requests_.begin();
                promise = it->second;
                pending_meta_requests_.erase(it);
            }
        }
        if(promise) {
            UploadMetaResult result;
            result.success = resp.success();
            result.is_duplicate = resp.is_duplicate();
            result.file_id = resp.file_id();
            result.existing_file_id = resp.existing_file_id();
            result.resume_offset = resp.uploaded_size();
            result.error_msg = resp.message();
            promise->set_value(result);
        }

        if(msg_callback_) {
            if(resp.is_duplicate()) {
                msg_callback_("[文件] 已有相同文件，ID: " + resp.existing_file_id());
            }
            else if(resp.success()) {
                msg_callback_("[文件] 上传准备就绪，file_id=" + resp.file_id() +
                            ", 已上传=" + std::to_string(resp.uploaded_size()));
            }
            else {
                msg_callback_("[错误] 上传失败: " + resp.message());
            }
        }
    }

    // 文件上传完成
    void handleFileUploadComplete(const db::FileUploadComplete& complete) {
        if(msg_callback_) {
            if(complete.success()) {
                msg_callback_("[文件] 上传完成: " + complete.file_id() +
                            " 大小=" + std::to_string(complete.file_size()));
            }
            else {
                msg_callback_("[错误] 上传完成失败: " + complete.file_id());
            }
        }
    }

    // 文件下载响应
    void handleFileDownloadResp(const db::FileDownloadResp& resp) {
        if(msg_callback_) {
            if(resp.success()) {
                download_filename_map_[resp.file_id()] = resp.filename();
                msg_callback_("[文件] 开始下载: " + resp.filename() +
                            " (ID: " + resp.file_id() + ") 大小=" + std::to_string(resp.file_size()));
            }
            else {
                msg_callback_("[错误] 下载请求失败: " + resp.message());
            }
        }
    }

    // 接收下载分块
    void saveDownloadChunk(const db::FileDownloadChunk& chunk) {
        // 转换为FileChunk
        db::FileChunk generic_chunk;
        generic_chunk.set_file_id(chunk.file_id());
        generic_chunk.set_chunk_index(chunk.chunk_index());
        generic_chunk.set_offset(chunk.offset());
        generic_chunk.set_data(chunk.data());
        generic_chunk.set_is_last(chunk.is_last());
        saveChunkToFile(generic_chunk);
    }

    // 断点续传响应
    void handleFileResumeResp(const db::FileResumeResp& resp) {
        // 解析等待中的续传请求）
        std::shared_ptr<std::promise<ResumeResult>> promise;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            if(!pending_requests_.empty()) {
                auto it = pending_requests_.begin();
                promise = it->second;
                pending_requests_.erase(it);
            }
        }
        if(promise) {
            ResumeResult result;
            result.success = resp.success();
            result.offset = resp.offset();
            result.error_code = 0;
            promise->set_value(result);
        }
    }

    // 离线文件列表响应
    void handleOfflineFileListResp(const db::FileOfflineListResp& resp) {
        if(msg_callback_) {
            if(resp.success()) {
                std::string out = "离线文件列表 (" + std::to_string(resp.total_count()) + "个):";
                for(const auto& f : resp.files()) {
                    out += "\n " + f.filename() + " (" + f.file_id() + ") " +
                        std::to_string(f.file_size()) + " 字节";
                }
                msg_callback_(out);
            }
            else {
                msg_callback_("[错误] 获取离线文件列表失败");
            }
        }
    }

    void handleDeleteAccountResponse(const p::CommonResponse& resp) {
        LOG_INFO << "handleDeleteAccountResponse: code=" << resp.code() << ", msg=" << resp.message();
        std::lock_guard<std::mutex> lock(delete_mutex_);
        delete_response_received_ = true;
        delete_success_ = (resp.code() == 0);

        if(!delete_success_) {
            if(msg_callback_) {
                msg_callback_("[错误] 删除失败: " + resp.message());
            }
        }
        delete_cv_.notify_one();
    }

    void setUICallback(UICallback cb) {
        ui_callback_ = cb;
    }

    void getOfflineMessages() {
        p::GetOfflineMessagesRequest req;
        req.set_limit(100);
        req.set_before_time(0);
        
        p::MessageHeader header;
        header.set_msg_type(p::MSG_GET_OFFLINE_MESSAGES);
        header.set_timestamp(tool::getTimestamp());
        header.set_from_uid(user_.uid);
        header.set_to_uid(user_.uid);
        
        auto data = proto::MessageCodec::encode(header, req);
        if (!data.empty()) {
            sendData(data);
            LOG_DEBUG << "Requesting offline messages";
        }
    }

    void setCurMod(uint64_t target, bool is_group) {
        cur_target = target;
        cur_mod = is_group ? MODE_GROUP : MODE_PRIVATE;
        if(msg_callback_) {
            std::string type = is_group ? "群组" : "用户";
            msg_callback_("[系统] 已切换到" + type + "聊天， 目标用户: " + std::to_string(target) + "(直接发送消息即可) (输入/back退出)");
        }
        // 进入会话时标记该会话已读
        if(is_group) {
            markGroupRead(target);
        } else {
            markRead(target);
        }
    }

    void cleanChatMod() {
        cur_mod = MODE_NONE;
        cur_target = 0;
        if(msg_callback_) {
            msg_callback_("[系统] 退出当前的聊天, 回到用户界面");
        }
    }

    bool isInChatMode() const{
        return cur_mod != MODE_NONE;
    }

    bool isInPrivate() const{
        return cur_mod == MODE_PRIVATE;
    }

    bool isInGroupChat() const {
        return cur_mod == MODE_GROUP;
    }

    uint64_t getCurTarget() const {
        return cur_target;
    }

private:
    std::unique_ptr<TLSClient> client_;
    std::thread recv_thread_;
    std::mutex login_mutex_;
    std::mutex file_mutex_;
    std::condition_variable login_cv_;
    std::string host_;
    uint16_t port_ = 0;
    bool use_tls_ = false;
    std::string cert_file_;
    std::string key_file_;
    std::vector<ConversationInfo> conversation_list_;
    std::vector<uint64_t> block_list_;
    std::unordered_map<std::string, std::ofstream> downloading_files_;
    int epoll_fd_ {-1};
    bool running = false;
    std::atomic<bool> pre_login_heartbeat_running_{false};
    std::thread pre_login_heartbeat_thread_;
    std::mutex pre_login_mutex_;
    std::condition_variable pre_login_cv_;

    UserInfo user_;

    bool login_response_received_ = false;
    bool login_success_ = false;

    MessageCallback msg_callback_;
    UICallback ui_callback_;

    int heartbeat_interval_ = 15;
    std::atomic<bool> heartbeat_running_{false};
    std::atomic<bool> connected_{false};
    std::thread heartbeat_thread_;
    std::mutex heartbeat_mutex_;
    std::condition_variable heartbeat_cv_;
    uint64_t heartbeat_seq_ = 0;

    std::condition_variable delete_cv_;
    std::mutex delete_mutex_;
    bool delete_response_received_ = false;
    bool delete_success_ = false;

    std::atomic<uint64_t> next_request_id_{0};
    std::mutex pending_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<std::promise<ResumeResult>>> pending_requests_;

    std::mutex upload_meta_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<std::promise<UploadMetaResult>>> pending_meta_requests_;
    std::mutex chunk_ack_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<std::promise<bool>>> pending_chunk_acks_;
    std::unordered_map<std::string, std::string> download_filename_map_;

    enum ChatMod {
        MODE_NONE,
        MODE_PRIVATE,
        MODE_GROUP
    };
    ChatMod cur_mod = MODE_NONE;
    uint64_t cur_target = 0; // 目标ID

    void handleLoginResponse(const p::LoginResponse& resp) {
        std::lock_guard<std::mutex> lock(login_mutex_);
        login_response_received_ = true;
        login_success_ = resp.success();
        if(resp.success()) {
            user_.uid = resp.uid();
            user_.nickname = resp.nickname();
            user_.tID = resp.token();
            user_.logged_in = true;

            if(msg_callback_) {
                msg_callback_("[系统] 登录成功， 用户：" + resp.nickname() + ", 用户ID：" + std::to_string(user_.uid));
            }

            // stopPreLoginHeartbeat();
            // startHeartbeat(15);
            
            getFriendList();
            getConversationList();

        }
        else {
            if(msg_callback_) {
                msg_callback_("[错误] 登录失败：" + resp.message());
            }
        }

        login_cv_.notify_one();
    }

    void handleRegisterResponse(const p::CommonResponse& resp) {
        if(msg_callback_) {
            msg_callback_("[注册]" + std::string(resp.code() == 0 ? "成功" : "失败") + ": " + resp.message());
        }
    }

    void handlePrivateChat(const p::ChatMessage& msg) {
        if(msg_callback_) {
            uint64_t from = msg.from_uid();
            uint64_t msg_id = msg.msg_id();
            std::string from_str = (from == user_.uid) ? "我" : std::to_string(from);
            std::string content = msg.content();

            if (from == 0) {
                if (!content.empty()) {
                    // 显示离线消息内容
                    msg_callback_("[离线消息] " + content);
                } else {
                    // 空内容可能是通知
                    LOG_DEBUG << "Empty offline message notification";
                }
                return;
            }

            if (content.empty()) {
                content = "(空消息)";
            }

            msg_callback_("[私聊] " + from_str + ": " + content + "(ID: " + std::to_string(msg_id) + ")");

            // 收到消息时若正与对方聊天，则自动标记已读
            if (from != user_.uid && isInPrivate() && getCurTarget() == from) {
                markRead(from);
            }
        }
    }

    void handleGroupPush(const p::GroupMessagePush& push) {
        LOG_DEBUG << "=== GroupMessagePush Debug ===";
        
        std::string debug_str;
        google::protobuf::util::MessageToJsonString(push, &debug_str);
        LOG_DEBUG << "JSON representation: " << debug_str;
        
        const auto* descriptor = push.GetDescriptor();
        const auto* reflection = push.GetReflection();
        for (int i = 0; i < descriptor->field_count(); ++i) {
            const auto* field = descriptor->field(i);
            if (reflection->HasField(push, field)) {
                LOG_DEBUG << "Field " << field->name() << " (number " << field->number() 
                        << ") is set";
            } else {
                LOG_DEBUG << "Field " << field->name() << " (number " << field->number() 
                        << ") is NOT set";
            }
        }
              
        if(msg_callback_) {
            uint64_t from_uid = push.from_uid();
            std::string content = push.content();
            uint64_t group_id = push.group_id();
            if (content.empty()) {
                content = "(空消息)";
            }
            std::string display_content = content.empty() ? "(空消息)" : content;
            msg_callback_("[群聊] " + std::to_string(push.group_id()) + " " + std::to_string(from_uid) + ": " + content);

            // 收到群消息时若正查看该群，则自动标记已读
            if (isInGroupChat() && getCurTarget() == group_id) {
                markGroupRead(group_id);
            }
        }
    }

    void handleGroupChatMessage(const p::GroupChatMessage& msg) {
        if(msg_callback_) {
            std::string from = std::to_string(msg.from_uid());
            std::string content = msg.content();
            if (content.empty()) {
                content = "(空消息)";
            }
            // 注意：GroupChatMessage 中的字段是 group_uid
            msg_callback_("[群聊] " + std::to_string(msg.group_uid()) + " " + from + ": " + content);

            // 离线群消息同样标记已读
            if (isInGroupChat() && getCurTarget() == msg.group_uid()) {
                markGroupRead(msg.group_uid());
            }
        }
    }

    void handleConversationListResponse(const p::ConversationListResponse& resp) {
        conversation_list_.clear();

        if(!resp.success()) {
            if(msg_callback_) {
                msg_callback_("[错误] 获取会话列表失败");
            }
            return ;
        }

        for(int i = 0; i < resp.conversations_size(); i++) {
            const auto& conv = resp.conversations(i);

            ConversationInfo info;
            info.user_id = conv.user_id();
            info.username = conv.username();
            info.nickname = conv.nickname();
            info.avatar = conv.avatar();
            info.last_msg_content = conv.last_msg_content();
            info.last_msg_time = conv.last_msg_time();
            info.unread_count = conv.unread_count();
            info.online_status = conv.online_status();

            conversation_list_.push_back(info);
        }

        if(msg_callback_) {
            std::string out = "会话列表 (" + std::to_string(conversation_list_.size()) + "个):";
            for(const auto& conv : conversation_list_) {
                std::string status = conv.online_status ? "[在线]" : "[离线]";
                std::string unread = conv.unread_count > 0 ? " (" + std::to_string(conv.unread_count) + "条未读)" : "";
                out += "\n " + status + " " + conv.nickname + unread;
                if(!conv.last_msg_content.empty()) {
                    out += "\n " + conv.last_msg_content;
                }
            }
            msg_callback_(out);
        }
        LOG_DEBUG << "Received " << conversation_list_.size() << " conversations";
    }
};