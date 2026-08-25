#pragma once

#include "protobuf/mysql.pb.h"
#include "protobuf/p.h"
#include "mysql/fileDAO.h"
#include "tool/logging.h"
#include "tool/tool.h"
#include "epoll.h"
#include "fileManage.h"
#include <unordered_map>
#include <mutex>
#include <map>
#include <vector>
#include "mysql/messageDAO.h"
#include "mysql/groupmessageDAO.h"
#include <groupDAO.h>

// 文件传输会话
struct FileSession {
    std::string file_id;
    uint64_t from_uid;
    uint64_t to_uid;
    uint64_t group_id;
    uint64_t total_size;
    uint64_t uploaded_size;
    uint64_t download_size;
    int direction; // 0.上传  1.下载
    std::chrono::steady_clock::time_point last_active;
    bool is_offline;
};


class FileHandle {
public:
    FileHandle() {
        manage_.init("./files", 7);
        manage_.startClean();
    }

    ~FileHandle() {
        manage_.stopCleaner();
    }

    void setUserConnections(std::unordered_map<uint64_t, std::shared_ptr<TcpConnection>>* conn) {
        user_connection_ = conn;
    }

    // 文件的上传请求
    void handleFileUploadRequest(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        db::FileUploadReq req;
        if(!req.ParseFromArray(body.data(), body.size())) {
            sendFileResponse(conn, header, false, "Invalid request");
            return ;
        }
        
        uint64_t from_uid = conn->getUserID();
        if(from_uid == 0) {
            sendFileResponse(conn, header, false, "User not logged in");
            return ;
        }

        const auto& info = req.file_info();
        std::string file_id = info.file_id();
        uint64_t group_id = info.group_id();
        bool is_group_chat = (group_id != 0);

        if(is_group_chat) {
            GroupDAO group_dao;
            if (!group_dao.isGroupMember(group_id, from_uid)) {
                sendFileResponse(conn, header, false, "Not a member of this group");
                return;
            }
        }
        else {
            if (info.to_uid() == 0) {
                sendFileResponse(conn, header, false, "Missing target user");
                return;
            }
        }

        if(file_id.empty()) {
            file_id = manage_.generateFileID();
        }

        // MD5去重
        FileDAO dao;
        FileRecord existing;
        std::string existing_file_id;

        if(!info.md5().empty()) {
            // 仅当数据库中已存在同 MD5 记录、且其物理文件确实存在时才视为重复
            if(dao.getFileByMD5(info.md5(), existing) && manage_.fileExists(existing.file_id)) {
                existing_file_id = existing.file_id;
                LOG_INFO << "Duplicate file detected, MD5: " << info.md5() << ", existing file: " << existing.file_id;

                // 判断是否已存在"相同发送者 + 相同接收方"的投递记录
                bool same_delivery = (existing.from_uid == from_uid) &&
                                     (existing.to_uid == info.to_uid()) &&
                                     (existing.group_id == group_id);

                if(same_delivery) {
                    // 完全重复：复用已有记录，仅通知接收方
                    db::FileUploadResp duplicate_resp;
                    duplicate_resp.set_success(true);
                    duplicate_resp.set_file_id(existing_file_id);
                    duplicate_resp.set_message("已有相同文件: " + existing_file_id);
                    duplicate_resp.set_uploaded_size(existing.file_size);
                    duplicate_resp.set_is_duplicate(true);
                    duplicate_resp.set_existing_file_id(existing_file_id);

                    p::MessageHeader resp_header;
                    resp_header.set_msg_id(header.msg_id() + 1);
                    resp_header.set_msg_type(p::MSG_FILE_UPLOAD_RESP);
                    resp_header.set_timestamp(tool::getTimestamp());
                
                    auto data = proto::MessageCodec::encode(resp_header, duplicate_resp);
                    if(!data.empty()) {
                        conn->send(data.data(), data.size());
                    }

                    FileRecord notify_record = existing;
                    notify_record.from_uid = from_uid;
                    notify_record.to_uid = info.to_uid();
                    notify_record.group_id = group_id;
                    notify_record.is_offline = false;
                    if(is_group_chat) {
                        sendGroupFileMessage(notify_record, group_id);
                    }
                    else {
                        sendFileMessage(notify_record, header);
                    }
                }
                else {
                    // 相同内容投递给新接收方：新建记录并硬链接复用物理文件，确保新接收方能下载
                    std::string ext;
                    size_t dot = info.filename().find_last_of('.');
                    if(dot != std::string::npos && dot + 1 < info.filename().size()) {
                        ext = info.filename().substr(dot + 1);
                    }

                    std::string new_file_id = manage_.generateFileID();

                    FileRecord new_record = existing;
                    new_record.file_id = new_file_id;
                    new_record.filename = info.filename();
                    new_record.file_size = info.file_size();
                    new_record.md5 = info.md5();
                    new_record.mime_type = info.mime_type();
                    new_record.upload_time = tool::getTimestamp();
                    new_record.expire_time = new_record.upload_time + 7 * 24 * 3600 * 1000;
                    new_record.from_uid = from_uid;
                    new_record.to_uid = info.to_uid();
                    new_record.group_id = group_id;
                    new_record.status = 1;   // 直接复用已有物理文件，视为完成
                    new_record.is_offline = false;
                    new_record.local_path = manage_.getFilePathWithExt(new_file_id, ext);

                    std::string src_path = manage_.getFilePath(existing_file_id); // 能正确找到源文件（带扩展名）
                    std::string dst_path = manage_.getFilePathWithExt(new_file_id, ext);

                    std::error_code ec;
                    std::filesystem::create_hard_link(src_path, dst_path, ec);
                    if (ec) {
                        // 硬链接失败（如跨设备），回退为复制
                        LOG_WARN << "Hard link failed, fallback to copy: " << ec.message();
                        std::filesystem::copy_file(src_path, dst_path, std::filesystem::copy_options::overwrite_existing, ec);
                        if (ec) {
                            LOG_ERROR << "Failed to copy file for deduplication: " << ec.message();
                            sendFileResponse(conn, header, false, "File copy failed");
                            return;
                        }
                        LOG_INFO << "File copied (fallback): " << src_path << " -> " << dst_path;
                    } else {
                        LOG_INFO << "File linked: " << src_path << " -> " << dst_path;
                    }

                    if(!dao.insertFile(new_record)) {
                        sendFileResponse(conn, header, false, "Database error");
                        return;
                    }

                    if(is_group_chat) {
                        sendGroupFileMessage(new_record, group_id);
                    }
                    else {
                        sendFileMessage(new_record, header);
                    }

                    // 告知客户端新文件 ID
                    db::FileUploadResp duplicate_resp;
                    duplicate_resp.set_success(true);
                    duplicate_resp.set_file_id(new_file_id);
                    duplicate_resp.set_message("已有相同文件: " + existing_file_id);
                    duplicate_resp.set_uploaded_size(existing.file_size);
                    duplicate_resp.set_is_duplicate(true);
                    duplicate_resp.set_existing_file_id(existing_file_id);

                    p::MessageHeader resp_header;
                    resp_header.set_msg_id(header.msg_id() + 1);
                    resp_header.set_msg_type(p::MSG_FILE_UPLOAD_RESP);
                    resp_header.set_timestamp(tool::getTimestamp());
                
                    auto data = proto::MessageCodec::encode(resp_header, duplicate_resp);
                    if(!data.empty()) {
                        conn->send(data.data(), data.size());
                    }
                }
                return ;
            }
        }

        // 检查文件是否已经存在（断点续传）
        FileRecord record;
        bool exists = dao.getFileByID(file_id, record);

        // 创建或更新文件记录
        record.file_id = file_id;
        record.filename = info.filename();
        record.file_size = info.file_size();
        record.md5 = info.md5();
        record.mime_type = info.mime_type();
        record.upload_time = tool::getTimestamp();
        record.expire_time = record.upload_time + 7 * 24 * 3600 * 1000;
        record.from_uid = from_uid;
        record.to_uid = info.to_uid();
        record.group_id = group_id; 
        record.status = 0;
        std::string ext;
        size_t dot = info.filename().find_last_of('.');
        if (dot != std::string::npos && dot + 1 < info.filename().size()) {
            ext = info.filename().substr(dot + 1);
        }
        record.local_path = manage_.getFilePathWithExt(file_id, ext);
        record.is_offline = false;

        if(!exists) {
            if(!dao.insertFile(record)) {
                sendFileResponse(conn, header, false, "Database error");
                return ;
            }
        }
        else {
            dao.updateFileStatus(file_id, 0);
        }

        // 记录会话
        FileSession session;
        session.file_id = file_id;
        session.from_uid = from_uid;
        session.to_uid = info.to_uid();
        session.group_id = group_id;
        session.total_size = info.file_size();
        session.direction = 0;
        session.last_active = std::chrono::steady_clock::now();
        session.is_offline = false;

        uint64_t uploaded = manage_.getTempFileSize(file_id);
        session.uploaded_size = uploaded;

        // 限制锁的生命周期，写入数据 
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            upload_sessions_[file_id] = session;
        }

        // 响应
        db::FileUploadResp resp;
        resp.set_success(true);
        resp.set_file_id(file_id);
        resp.set_message("Upload ready");
        resp.set_uploaded_size(uploaded);
        resp.set_is_duplicate(false);

        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_FILE_UPLOAD_RESP);
        resp_header.set_timestamp(tool::getTimestamp());
        
        auto data = proto::MessageCodec::encode(resp_header, resp);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }

        LOG_INFO << "File upload started: " << file_id << " from " << from_uid << ", resume offset: " << uploaded; 
    }

    // 文件传输块
    void handleFileUploadChunk(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        uint64_t user_id = conn->getUserID();
        // if(user_id  > 0) {
        //     OnlineManager::getInstance().updateHeartbeat(user_id);
        // }

        db::FileUploadChunk chunk;
        if(!chunk.ParseFromArray(body.data(), body.size())) {
            LOG_ERROR << "Parse chunk failed";
            return ;
        }

        std::string file_id = chunk.file_id();
        const std::string& data = chunk.data();
        uint64_t offset = chunk.offset();

        // 验证会话
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = upload_sessions_.find(file_id);
        if(it == upload_sessions_.end()) {
            LOG_ERROR << "No upload session file: " << file_id;
            return ;
        }

        auto& session = it->second;

        // 保存文件块
        if(!manage_.saveChunk(file_id, offset, data)) {
            LOG_ERROR << "Save chunk failed for file: " << file_id;
            return ;
        }

        session.uploaded_size = offset + data.size();
        session.last_active = std::chrono::steady_clock::now();

        // 如果是最后一块，完成上传
        if(chunk.is_last()) {
            //OnlineManager::getInstance().updateHeartbeat(user_id);
            std::string expected_md5;
            FileDAO dao;
            FileRecord record;
            if(dao.getFileByID(file_id, record)) {
                expected_md5 = record.md5;
            }

            std::string ext;
            size_t dot = record.filename.find_last_of('.');
            if (dot != std::string::npos && dot + 1 < record.filename.size()) {
                ext = record.filename.substr(dot + 1);
            }

            if(manage_.completeUpload(file_id, expected_md5, ext)) {
                // 更新数据库状态
                dao.updateFileStatus(file_id, 1);
                dao.updateFilePath(file_id, manage_.getFilePathWithExt(file_id, ext));

                // 获取完整的文件信息
                if(dao.getFileByID(file_id, record)) {
                    // 查看接受方是否在线
                    if (record.group_id != 0) {
                        // 群聊处理
                        sendGroupFileMessage(record, record.group_id);
                        // 保存群聊离线消息
                    } else {
                        // 私聊处理
                        bool is_online = false;
                        if(user_connection_) {
                            auto it_conn = user_connection_->find(record.to_uid);
                            if(it_conn != user_connection_->end()) {
                                is_online = true;
                                sendFileMessage(record, header);
                            }
                        }
                        // 离线
                        if(!is_online) {
                            dao.saveOfflineFile(file_id, record.to_uid);
                            LOG_INFO << "File saved as offline for user " << record.to_uid;
                        }
                    }
                }

                // 响应
                db::FileUploadComplete complete;
                complete.set_file_id(file_id);
                complete.set_success(true);
                complete.set_md5(expected_md5);
                complete.set_file_size(session.total_size);

                p::MessageHeader resp_header;
                resp_header.set_msg_id(header.msg_id() + 1);
                resp_header.set_msg_type(p::MSG_FILE_UPLOAD_COMPLETE);
                resp_header.set_timestamp(tool::getTimestamp());
            
                auto data = proto::MessageCodec::encode(resp_header, complete);
                if(!data.empty()) {
                    conn->send(data.data(), data.size());
                }

                // 移除会话
                upload_sessions_.erase(it);
                LOG_INFO << "File upload complete: " << file_id << ", size: " << session.total_size;
            }
            else {
                LOG_ERROR << "File upload complete failed: " << file_id;
            }
        }
    }

    void markConnectionBusy(uint64_t user_id) {
        std::lock_guard<std::mutex> lock(busy_mutex_);
        busy_connections_.insert(user_id);
    }

    void markConnectionIdle(uint64_t user_id) {
        std::lock_guard<std::mutex> lock(busy_mutex_);
        busy_connections_.erase(user_id);
    }

    bool isConnectionBusy(uint64_t user_id) {
        std::lock_guard<std::mutex> lock(busy_mutex_);
        return busy_connections_.find(user_id) != busy_connections_.end();
    }

    // 文件的下载请求
    void handleFileDownloadReq(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        uint64_t user_id = conn->getUserID();
        // if(user_id > 0) {
        //     OnlineManager::getInstance().updateHeartbeat(user_id);
        // }
        
        db::FileDownloadReq req;
        if(!req.ParseFromArray(body.data(), body.size())) {
            sendFileResponse(conn, header, false, "Invalid request");
            return ;
        }

        if(user_id == 0) {
            sendFileResponse(conn, header, false, "User not logged in");
            return ;
        }

        std::string file_id = req.file_id();
        uint64_t offset = req.offset();

        FileDAO dao;
        FileRecord record;
        if(!dao.getFileByID(file_id, record)) {
            sendFileResponse(conn, header, false, "File not found");
            return ;
        }

        // 验证
        if (record.group_id != 0) {
            // 群聊文件：检查用户是否在群组中
            GroupDAO group_dao;
            if (!group_dao.isGroupMember(record.group_id, user_id)) {
                sendFileResponse(conn, header, false, "Permission denied: not in group");
                return;
            }
        }
        else {
            // 私聊文件：仅接收者可下载
            if (record.to_uid != user_id && record.from_uid != user_id) {
                sendFileResponse(conn, header, false, "Permission denied");
                return;
            }
        }

        // 检查文件是否存在
        uint64_t file_size = manage_.getFileSize(file_id);
        if(file_size == 0) {
            sendFileResponse(conn, header, false, "File data missing");
            return ;
        }

        // 创建下载会话
        FileSession session;
        session.file_id = file_id;
        session.from_uid = record.from_uid;
        session.to_uid = user_id;
        session.group_id = record.group_id;
        session.total_size = file_size;
        session.download_size = offset;
        session.direction = 1;
        session.last_active = std::chrono::steady_clock::now();
        session.is_offline = record.is_offline;
    
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            download_sessions_[file_id] = session;
        }

        // 响应
        db::FileDownloadResp resp;
        resp.set_success(true);
        resp.set_file_id(file_id);
        resp.set_file_size(file_size);
        resp.set_filename(record.filename);
        resp.set_offset(offset);
        resp.set_message("Download ready");
        resp.set_md5(record.md5);
        resp.set_is_offline(record.is_offline);

        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_FILE_DOWNLOAD_RESP);
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, resp);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }

        markConnectionBusy(user_id);

        // 在独立线程中传输文件块，避免阻塞 reactor 线程导致心跳无法处理
        std::thread([this, conn, file_id, offset, file_size]() {
            //OnlineManager::getInstance().updateHeartbeat(conn->getUserID());
            sendNextChunk(conn, file_id, offset, file_size);
            //OnlineManager::getInstance().updateHeartbeat(conn->getUserID());
        }).detach();

        LOG_INFO << "File download started: " << file_id << " for user " << user_id << ", offset: " << offset;
    }

    // 发送文件块（独立线程中执行，带背压；输出缓冲满时短暂休眠等待刷出）
    void sendNextChunk(std::shared_ptr<TcpConnection> conn, const std::string& file_id, uint64_t offset, uint64_t file_size) {
        const uint32_t CHUNK_SIZE = 64 * 1024;
        const size_t MAX_BUFFERED = 256 * 1024;
        uint64_t user_id = conn->getUserID();
        const int CHUNKS_PER_HEARTBEAT = 20;
        //int heartbeat_chunk = 0;

        while (true) {
            if (!conn || conn->isClosed()) {
                LOG_ERROR << "Connection closed during download: " << file_id;
                {
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    download_sessions_.erase(file_id);
                }
                manage_.closeReadFile(file_id);
                markConnectionIdle(user_id);
                return;
            }

            std::string chunk_data;
            if (!manage_.readChunk(file_id, offset, CHUNK_SIZE, chunk_data)) {
                LOG_ERROR << "Read chunk failed at offset " << offset;
                break;
            }
            if (chunk_data.empty()) break;

            uint32_t chunk_index = offset / CHUNK_SIZE;

            db::FileDownloadChunk chunk;
            chunk.set_file_id(file_id);
            chunk.set_chunk_index(chunk_index);
            chunk.set_offset(offset);
            chunk.set_data(chunk_data);
            // 根据剩余字节数判定最后一块，正确处理文件大小为块大小整数倍的情况
            chunk.set_is_last(offset + chunk_data.size() >= file_size);

            p::MessageHeader header;
            header.set_msg_type(p::MSG_FILE_DOWNLOAD_CHUNK);
            header.set_timestamp(tool::getTimestamp());

            auto msg_data = proto::MessageCodec::encode(header, chunk);
            if (!msg_data.empty()) {
                conn->send(msg_data.data(), msg_data.size());
            }

            // 更新会话
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                auto it = download_sessions_.find(file_id);
                if (it != download_sessions_.end()) {
                    it->second.download_size = offset + chunk_data.size();
                    it->second.last_active = std::chrono::steady_clock::now();
                }
            }

            if (chunk.is_last()) {
                LOG_INFO << "File download completed: " << file_id;
                {
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    download_sessions_.erase(file_id);
                }
                manage_.closeReadFile(file_id);
                markConnectionIdle(user_id);

                // 传输完整完成后才标记离线文件已下载，避免中断导致无法重试
                FileDAO file_dao;
                file_dao.markOfflineFileDownLoaded(file_id, user_id);
                return;
            }

            offset += chunk_data.size();

            // 背压：输出缓冲超过阈值则休眠等待 reactor 刷出，避免内存暴涨
            while (conn->outputBufferSize() > MAX_BUFFERED) {
                if (conn->isClosed()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // heartbeat_chunk++;
            // if (heartbeat_chunk >= CHUNKS_PER_HEARTBEAT) {
            //     //OnlineManager::getInstance().updateHeartbeat(user_id);
            //     conn->updateActivityTime();
            //     heartbeat_chunk = 0;
            // }
        }

        // 提前读到 EOF（理论上 is_last 已覆盖），按完成收尾
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            download_sessions_.erase(file_id);
        }
        manage_.closeReadFile(file_id);
        markConnectionIdle(user_id);
    }

    // 发送文件消息给接收方
    void sendFileMessage(const FileRecord& record, const p::MessageHeader& req_header) {
        uint64_t to_uid = record.to_uid;
        bool is_online = false;

        if(user_connection_) {
            auto it = user_connection_->find(to_uid);
            if(it != user_connection_->end()) {
                is_online = true;
                db::FileMessage file_msg;
                file_msg.set_file_id(record.file_id);
                file_msg.set_filename(record.filename);
                file_msg.set_file_size(record.file_size);
                file_msg.set_md5(record.md5);
                file_msg.set_from_uid(record.from_uid);
                file_msg.set_to_uid(record.to_uid);
                file_msg.set_send_time(tool::getTimestamp());
                file_msg.set_is_offline(false);

                p::MessageHeader header;
                header.set_msg_type(p::MSG_FILE_MESSAGE);
                header.set_from_uid(record.from_uid);
                header.set_to_uid(record.to_uid);
                header.set_timestamp(tool::getTimestamp());

                auto data = proto::MessageCodec::encode(header,file_msg);
                if(!data.empty()) {
                    it->second->send(data.data(), data.size());
                }
                
                LOG_INFO << "File message sent to " << to_uid;
            }
        }

        // 如果不在线,则保存为离线文件
        if(!is_online) {
            FileDAO dao;
            dao.saveOfflineFile(record.file_id, to_uid);
            LOG_INFO << "File saved as offline for user " << to_uid;
        }
    }
    
    void sendGroupFileMessage(const FileRecord& record, uint64_t group_id) {
        // 构造文件元数据
        db::FileMessage file_meta;
        file_meta.set_file_id(record.file_id);
        file_meta.set_filename(record.filename);
        file_meta.set_file_size(record.file_size);
        file_meta.set_md5(record.md5);
        file_meta.set_from_uid(record.from_uid);
        file_meta.set_to_uid(0);
        file_meta.set_send_time(tool::getTimestamp());
        file_meta.set_is_offline(false);

        // 保存群聊消息到 group_messages
        GroupMessage group_msg;
        group_msg.group_id = group_id;
        group_msg.from_uid = record.from_uid;
        group_msg.msg_type = 3;                      // 文件类型
        group_msg.content = record.filename;
        group_msg.extra = Switch::sToJson(file_meta);
        group_msg.status = 0;
        group_msg.created_at = tool::getTimestamp();

        GroupMessageDAO group_msg_dao;
        uint64_t msg_id;
        if (!group_msg_dao.saveGroupMessage(group_msg, msg_id)) {
            LOG_ERROR << "Failed to save group file message";
            return;
        }

        // 获取群成员
        GroupDAO g_dao;
        auto members = g_dao.getGroupMembers(group_id);

        for (const auto& member : members) {
            if (member.user_id == record.from_uid) continue; // 不发给发送者
            bool is_online = false;
            if (user_connection_) {
                auto it = user_connection_->find(member.user_id);
                if (it != user_connection_->end()) {
                    is_online = true;
                    // 推送文件消息
                    p::MessageHeader push_header;
                    push_header.set_msg_type(p::MSG_FILE_MESSAGE);
                    push_header.set_from_uid(record.from_uid);
                    push_header.set_to_uid(group_id);
                    push_header.set_timestamp(tool::getTimestamp());
                    auto data = proto::MessageCodec::encode(push_header, file_meta);
                    if (!data.empty()) {
                        it->second->send(data.data(), data.size());
                    }
                }
            }
            if (!is_online) {
                group_msg_dao.saveGroupOfflineMessage(member.user_id, group_id, msg_id);
            }
        }
        LOG_INFO << "Group file message sent to " << members.size() << " members, msg_id=" << msg_id;
    }

    // 文件断点续传请求
    void handleFileResumeReq(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        db::FileResumeReq req;
        if(!req.ParseFromArray(body.data(), body.size())) {
            sendFileResponse(conn, header, false, "Invalid request");
            return ;
        }

        std::string file_id = req.file_id();
        int direction = req.direction();

        db::FileResumeResp resp;
        if(direction == 0) { // 上传
            uint64_t uploaded = manage_.getTempFileSize(file_id);
            resp.set_success(true);
            resp.set_offset(uploaded);
            resp.set_message("Resume upload");
        }
        else {  //下载
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = download_sessions_.find(file_id);
            if(it != download_sessions_.end()) {
                resp.set_success(true);
                resp.set_offset(it->second.download_size);
                resp.set_message("Resume download");
            }
            else {
                // 没有进行中的下载会话，属于全新下载，从偏移 0 开始
                resp.set_success(true);
                resp.set_offset(0);
                resp.set_message("No download session, start from 0");
            }
        }

        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_FILE_RESUME_RESP);
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, resp);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }

    // 获取离线文件列表
    void handleOfflineFiles(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        uint64_t user_id = conn->getUserID();
        if(user_id == 0) {
            sendFileResponse(conn, header, false, "User not logged in");
            return ;  
        }

        FileDAO dao;
        auto offline_records = dao.getOfflineFiles(user_id);
        //  获取离线文件的详细信息
        std::vector<FileRecord> file_records;
        for(const auto& rec : offline_records) {
            FileRecord file_rec;
            if(dao.getFileByID(rec.file_id, file_rec)) {
                // 检查文件是否存在
                if(manage_.fileExists(rec.file_id)) {
                    file_records.push_back(file_rec);
                }
                else {
                    dao.deleteOfflineFileRecord(rec.file_id, user_id);
                }
            }
        }

        db::FileOfflineListResp resp;
        resp.set_success(true);
        resp.set_total_count(file_records.size());

        for(const auto& rec : file_records) {
            auto* file_info = resp.add_files();
            file_info->set_file_id(rec.file_id);
            file_info->set_filename(rec.filename);
            file_info->set_file_size(rec.file_size);
            file_info->set_md5(rec.md5);
            file_info->set_mime_type(rec.mime_type);
            file_info->set_upload_time(rec.upload_time);
            file_info->set_expire_time(rec.expire_time);
            file_info->set_from_uid(rec.from_uid);
            file_info->set_to_uid(rec.to_uid);
            file_info->set_status(rec.status);
            file_info->set_is_offline(true);
        }

        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(p::MSG_FILE_OFFLINE_DOWNLOAD);
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, resp);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }

        LOG_INFO << "Offline files for user " << user_id << ": " << file_records.size();
    }


private:
    FileManage manage_;
    std::unordered_map<uint64_t, std::shared_ptr<TcpConnection>>* user_connection_ = nullptr;
    std::mutex sessions_mutex_;
    std::unordered_map<std::string, FileSession> upload_sessions_;
    std::unordered_map<std::string, FileSession> download_sessions_;
    std::mutex busy_mutex_;
    std::unordered_set<uint64_t> busy_connections_;


    void sendFileResponse(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, bool success, const std::string& msg) {
        p::CommonResponse resp;
        resp.set_code(success ? 0 : -1);
        resp.set_message(msg);
        resp.set_timestamp(tool::getTimestamp());
    
        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(header.msg_type());
        resp_header.set_timestamp(tool::getTimestamp());
    
        auto data = proto::MessageCodec::encode(resp_header, resp);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }
};