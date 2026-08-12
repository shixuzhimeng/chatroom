#pragma once

#include "protobuf/mysql.pb.h"
#include "protobuf/p.h"
#include "mysql/fileDAO.h"
#include "../logging.h"
#include "../tool.h"
#include "epoll.h"
#include "FileManage.h"
#include <unordered_map>
#include <mutex>
#include <map>
#include <vector>

// 文件传输会话
struct FileSession {
    std::string file_id;
    uint64_t from_uid;
    uint64_t to_uid;
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

    void setUserConnection(std::unordered_map<uint64_t, std::shared_ptr<TcpConnection>>* conn) {
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
                    sendFileMessage(new_record, header);
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
        if(it != upload_sessions_.end()) {
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
            if(!dao.getFileByID(file_id, record)) {
                expected_md5 = record.md5;
            }

            if(manage_.completeUpload(file_id, expected_md5)) {
                // 更新数据库状态
                dao.updateFileStatus(file_id, 1);
                dao.updateFilePath(file_id, manage_.getFilePath(file_id));

                // 获取完整的文件信息
                if(dao.getFileByID(file_id, record)) {
                    // 查看接受方是否在线
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
        }
    }


private:
    FileManage manage_;
    std::unordered_map<uint64_t, std::shared_ptr<TcpConnection>>* user_connection_;
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