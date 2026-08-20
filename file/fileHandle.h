#pragma once

#include "protobuf/mysql.pb.h"
#include "protobuf/p.h"
#include "mysql/fileDAO.h"
#include "../logging.h"
#include "../tool.h"
#include "epoll.h"
#include "fileManage.h"
#include <unordered_map>
#include <mutex>
#include <map>
#include <vector>
#include "mysql/messageDAO.h"
#include "mysql/groupmessageDAO.h"

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
        bool is_duplicate = false;
        std::string existing_file_id;

        if(!info.md5().empty()) {
            if(dao.getFileByMD5(info.md5(), existing)) {
                is_duplicate = true;
                existing_file_id = existing.file_id;
                LOG_INFO << "Duplicate file deceted, MD5: " << info.md5() << " ,existing file: " << existing.file_id;

                // 如果文件重复，则将直接使用现存的文件
                db::FileUploadResp duplicate_resp;
                duplicate_resp.set_success(true);
                duplicate_resp.set_file_id(existing_file_id);
                duplicate_resp.set_message("File already exixts, using existing copy");
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
                
                // 创建文件只想已有的文件
                FileRecord new_record = existing;
                new_record.file_id = file_id;
                new_record.from_uid = from_uid;
                new_record.to_uid = info.to_uid();
                new_record.is_offline = false;
                // 复制文件
                if(manage_.copyFile(existing_file_id, file_id)) {
                    dao.insertFile(new_record);
                    if(is_group_chat) {
                        sendFileMessage(new_record, header);
                    }
                    else {
                        sendFileMessage(new_record, header);
                    }
                }
                else {
                    LOG_ERROR << "Failed to copy file for deduplication: " << existing_file_id;
                    sendFileResponse(conn, header, false, "File copy failed");
                    return;
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
        record.local_path = manage_.getFilePath(file_id);
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
            std::string expected_md5;
            FileDAO dao;
            FileRecord record;
            if(dao.getFileByID(file_id, record)) {
                expected_md5 = record.md5;
            }

            if(manage_.completeUpload(file_id, expected_md5)) {
                // 更新数据库状态
                dao.updateFileStatus(file_id, 1);
                dao.updateFilePath(file_id, manage_.getFilePath(file_id));

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

    // 文件的下载请求
    void handleFileDownloadReq(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        db::FileDownloadReq req;
        if(!req.ParseFromArray(body.data(), body.size())) {
            sendFileResponse(conn, header, false, "Invalid request");
            return ;
        }

        uint64_t user_id = conn->getUserID();
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

        // 标记离线文件已下载
        if(record.is_offline) {
            dao.markOfflineFileDownLoaded(file_id, user_id);
        }

        // 开始传输文件块
        sendNextChunk(conn, file_id, offset);

        LOG_INFO << "File download started: " << file_id << " for user " << user_id << ", offset: " << offset;
    }

    // 发送文件块
    void sendNextChunk(std::shared_ptr<TcpConnection> conn, const std::string& file_id, uint64_t offset) {
        const uint32_t CHUNK_SIZE = 64 * 1024;

        while (true) {
            if (!conn || conn->isClosed()) {
                LOG_ERROR << "Connection closed during download: " << file_id;
                break;
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
            chunk.set_is_last(chunk_data.size() < CHUNK_SIZE);

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
                break;
            }

            offset += chunk_data.size();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
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
                resp.set_success(false);
                resp.set_message("No downlaod session");
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