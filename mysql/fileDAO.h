#pragma once

#include "baseDAO.h"
#include "../logging.h"
#include "../tool.h"
#include <string>
#include <vector>
#include <map>
#include "protobuf/p.h"
#include "protobuf/mysql_p.h"

struct FileRecord {
    std::string file_id;
    std::string filename;
    uint64_t file_size = 0;
    std::string md5;
    std::string mime_type;
    int64_t upload_time = 0;
    int64_t expire_time = 0;
    uint64_t from_uid = 0;
    uint64_t to_uid = 0;
    int status = 0;         // 0.上传中  1.完成  2.过期  3.删除
    std::string extra;      // 序列化的 FileInfo
    std::string local_path;
    std::string text;
};

class FileDAO : public BaseDAO {
public:
    bool insertFile(const FileRecord& record) {
        // 序列化
        db::FileInfo info;
        info.set_file_id(record.file_id);
        info.set_filename(record.filename);
        info.set_file_size(record.file_size);
        info.set_md5(record.md5);
        info.set_mime_type(record.mime_type);
        info.set_upload_time(record.upload_time);
        info.set_expire_time(record.expire_time);
        info.set_from_uid(record.from_uid);
        info.set_to_uid(record.to_uid);
        info.set_status(record.status);
        info.set_text(record.text);
        std::string extra_json = Switch::sToJson(info);

        std::string sql = "INSERT INTO file_info (file_id, filename, file_size, md5, mime_type, "
                         "upload_time, expire_time, from_uid, to_uid, status, extra, local_path, text) VALUES ('";
        sql += escapeString(record.file_id) + "', '";
        sql += escapeString(record.filename) + "', ";
        sql += std::to_string(record.file_size) + ", '";
        sql += escapeString(record.md5) + "', '";
        sql += escapeString(record.mime_type) + "', ";
        sql += std::to_string(record.upload_time) + ", ";
        sql += std::to_string(record.expire_time) + ", ";
        sql += std::to_string(record.from_uid) + ", ";
        sql += std::to_string(record.to_uid) + ", ";
        sql += std::to_string(record.status) + ", '";
        sql += escapeString(extra_json) + "', '";
        sql += escapeString(record.local_path) + "', '";
        sql += escapeString(record.text) + "')";
    
        return executeUpdate(sql);
    }

    // 文件ID来查询文件
    bool getFileByID(const std::string& file_id, FileRecord& record) {
        std::string sql = "SELECT * FROM file_info WHERE file_id = '" + escapeString(file_id) + "'";
        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result) || result.empty()) {
            return false;
        }
        fillRecordFromMap(result[0], record);
        return true;
    }

    // 查询指定用户收到的文件列表
    bool getFileByTarget(uint64_t to_uid, std::vector<FileRecord>& records, int status = 1) {
        std::string sql = "SELECT * FROM file_info WHERE to_uid = " + std::to_string(to_uid) + 
                          " AND status = " + std::to_string(status) + 
                          " ORDER BY upload_time DESC";
        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result)) {
            return false;
        }

        for(const auto& row : result) {
            FileRecord rec;
            fillRecordFromMap(row, rec);
            records.push_back(rec);
        }

        return true;
    }

    // 更新文件状态
    bool updateFileStatus(const std::string& file_id, int status) {
        std::string sql = "UPDATE file_info SET status = " + std::to_string(status) + 
                          " WHERE file_id = '" + escapeString(file_id) + "'";
        
        return executeUpdate(sql);
    }

    // 更新文件路径
    bool updateFilePath(const std::string& file_id, const std::string& path) {
        std::string sql = "UPDATE file_info SET local_path = '" + escapeString(path) + 
                          "' WHERE file_id = '" + escapeString(file_id) + "'";

        return executeUpdate(sql);

    }

    // 获取过期的文件(用来清理)
    std::vector<FileRecord> getExpiredFile(int64_t cur_time) {
        std::vector<FileRecord> records;
        std::string sql = "SELECT * FROM file_info WHERE expire_time < " + std::to_string(cur_time) + 
                          " AND status = 1";

        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result)) {
            return records;
        }

        for(const auto& row : result) {
            FileRecord rec;
            fillRecordFromMap(row, rec);
            records.push_back(rec);
        }


        return records;
    }

    // 获取用户的离线文件列表
    std::vector<FileRecord> getOfflineFiles(uint64_t user_id) {
        std::vector<FileRecord> records;
        if (getFileByTarget(user_id, records)) {
            return records;
        }
        return {};
    }

    // 删除过期文件
    bool deleteFileRecord(const std::string& file_id) {
        std::string sql = "DELETE FROM file_info WHERE file_id = '" + escapeString(file_id) + "'";
        return executeUpdate(sql);
    }

    // 标记文件为删除状态
    bool markFileDeleted(const std::string& file_id) {
        return updateFileStatus(file_id, 3);
    }


private:
    void fillRecordFromMap(const std::map<std::string, std::string>& row, FileRecord& record) {
        record.file_id = row.at("file_id");
        record.filename = row.at("filename");
        record.file_size = std::stoull(row.at("file_size"));
        record.md5 = row.at("md5");
        record.mime_type = row.at("mime_type");
        record.upload_time = std::stoll(row.at("upload_time"));
        record.expire_time = std::stoll(row.at("expire_time"));
        record.from_uid = std::stoull(row.at("from_uid"));
        record.to_uid = std::stoull(row.at("to_uid"));
        record.status = std::stoi(row.at("status"));
        record.extra = row.at("extra");
        record.local_path = row.at("local_path");
        record.text = row.at("text");
    }
};