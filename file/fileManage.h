#pragma once

#include <string>
#include <fstream>
#include <filesystem>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include "tool/logging.h"
#include "tool/tool.h"
#include "mysql/fileDAO.h"
#include "md5.h"
class FileManage {
public:
    static FileManage& getInstance() {
        static FileManage instance;
        return instance;
    }

    FileManage() = default;
    ~FileManage() {
        stopCleaner();
    }

    // 初始化文件目录结构
    bool init(const std::string& base_dir, int expired_days = 7) {
        if(expired_days <= 0) {
            LOG_ERROR << "expired_days must be positive";
            return false;
        }

        std::filesystem::path base_path(base_dir);

        try {
            // 检查是否为目录
            if(std::filesystem::exists(base_path) && !std::filesystem::is_directory(base_path)) {
                LOG_ERROR << "base_dir exists but is not a directory: " << base_dir;
                return false; 
            }

            // 创建基础的目录
            if(!std::filesystem::exists(base_path)) {
                if(!std::filesystem::create_directories(base_path)) {
                    LOG_ERROR << "Failed to created base_dir: " << base_dir;
                    return false;
                }
            }

            // 使用path来拼接子目录
            std::filesystem::path temp_path = base_path / "temp";
            std::filesystem::path store_path = base_path / "store";

            // create_directories 在目录已存在时返回 false（非错误），需用 error_code 判断真实失败
            std::error_code ec;
            std::filesystem::create_directories(temp_path, ec);
            if(ec || !std::filesystem::is_directory(temp_path)) {
                LOG_ERROR << "Failed to create temp directory: " << temp_path
                          << (ec ? " (" + ec.message() + ")" : "");
                return false;
            }

            std::filesystem::create_directories(store_path, ec);
            if(ec || !std::filesystem::is_directory(store_path)) {
                LOG_ERROR << "Failed to create store directory: " << store_path
                          << (ec ? " (" + ec.message() + ")" : "");
                return false;
            }


            base_dir_ = base_path.string();
            temp_dir_ = temp_path.string();
            store_dir_ = store_path.string();
            expired_days_ = expired_days;

            LOG_INFO << "FileManage init, base_dir: " << base_dir_;
            return true;
        }
        catch(const std::filesystem::filesystem_error& e) {
            LOG_ERROR << "Filesystem error suring init: " << e.what();
            return false;
        }
    }

    // 生成文件的唯一标识
    std::string generateFileID() {
        return tool::randString(32);
    }

    // 获取文件的完整的路径（磁盘中的正式目录）
    std::string getFilePath(const std::string& file_id) {
        std::string path_no_ext = store_dir_ + "/" + file_id;
        if (std::filesystem::exists(path_no_ext)) {
            return path_no_ext;
        }
        // 查找以 file_id 开头且后面紧跟 '.' 的文件
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(store_dir_, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();
            if (name.rfind(file_id, 0) == 0 && name.size() > file_id.size() && name[file_id.size()] == '.') {
                return entry.path().string();
            }
        }
        return path_no_ext;  // 找不到就返回无扩展名路径（文件不存在）
    }

    std::string getFilePathWithExt(const std::string& file_id, const std::string& ext) {
        return store_dir_ + "/" + file_id + (ext.empty() ? "" : "." + ext);
    }

    // 获取临时目录
    std::string getTempPath(const std::string& file_id) {
        return temp_dir_ + "/" + file_id + ".tmp";
    }

    // 检查文件是否存在
    bool fileExists(const std::string& file_id) {
        return std::filesystem::exists(getFilePath(file_id));
    }

    // 获取临时文件大小
    uint64_t getTempFileSize(const std::string& file_id) {
        std::string path = getTempPath(file_id);
        if(!std::filesystem::exists(path)) {
            return 0;
        }
        return std::filesystem::file_size(path);
    }


    // 临时传输文件（复用已打开的文件句柄，避免每块重复 open/close）
    bool saveChunk(const std::string& file_id, uint64_t offset, const std::string& data) {
        std::lock_guard<std::mutex> lock(streams_mutex_);

        auto it = temp_streams_.find(file_id);
        if (it == temp_streams_.end()) {
            std::string path = getTempPath(file_id);
            std::fstream fs(path, std::ios::binary | std::ios::in | std::ios::out);
            if (!fs) {
                // 文件尚不存在（新上传），用 out 模式创建
                fs.clear();
                fs.open(path, std::ios::binary | std::ios::out);
                if (!fs) {
                    LOG_ERROR << "Failed to create temp file: " << path;
                    return false;
                }
            }
            it = temp_streams_.emplace(file_id, std::move(fs)).first;
        }

        std::fstream& file = it->second;

        // 校验 offset 连续性（以流当前写位置为准，避免每块 stat）
        file.seekp(0, std::ios::end);
        uint64_t cur_size = static_cast<uint64_t>(file.tellp());
        if (offset != cur_size) {
            LOG_ERROR << "Invalid chunk offset: expected " << cur_size << ", got " << offset;
            return false;
        }

        file.seekp(offset);
        file.write(data.data(), data.size());
        if (!file) {
            LOG_ERROR << "Failed to write chunk for file: " << file_id;
            return false;
        }

        return true;
    }

    // 将临时文件存储到最终的存储位置并且验证MD5
    bool completeUpload(const std::string& file_id, const std::string& expected_md5, const std::string& ext = "") {
        // 关闭并移除已打开的上传临时文件句柄，确保数据落盘后可被 rename
        {
            std::lock_guard<std::mutex> lock(streams_mutex_);
            auto it = temp_streams_.find(file_id);
            if (it != temp_streams_.end()) {
                it->second.close();
                temp_streams_.erase(it);
            }
        }

        std::string temp_path = getTempPath(file_id);
        std::string store_path = getFilePathWithExt(file_id, ext);

        // 计算临时文件的MD5
        std::string actual_md5 = MD5Tool::calculateFile(temp_path);
        if(!expected_md5.empty() && actual_md5 != expected_md5) {
            LOG_ERROR << "MD5 mismatch for file " << file_id << ", expected: " << expected_md5 << ", actual: " << actual_md5;
            std::filesystem::remove(temp_path);
            return false;
        }
        
        try {
            // 检查文件是否存在
            if(!std::filesystem::exists(temp_path)) {
                LOG_ERROR << "Temp file not found: " << temp_path;
                return false;
            }

            // 确保文件存在
            std::filesystem::create_directories(std::filesystem::path(store_path).parent_path());

            // 尝试使用rename(本设备最优选择)
            std::filesystem::rename(temp_path, store_path);
            LOG_INFO << "File upload completed, move to: " << store_path;
        }
        catch(const std::filesystem::filesystem_error& e) {
            // 跨设备出问题
            // 换为拷贝+删除
            if(e.code() == std::errc::cross_device_link) {
                LOG_ERROR << "Cross-device rename, fallback to other";
                try {
                    // 拷贝文件
                    std::filesystem::copy(temp_path, store_path, std::filesystem::copy_options::overwrite_existing);
                    
                    // 拷贝成功，删除临时文件
                    std::filesystem::remove(temp_path);
                    LOG_INFO << "File upload commpleted to: " << store_path;
                    return true;
                }
                catch(const std::exception& copy_ex) {
                    LOG_ERROR << "this failed: " << copy_ex.what();
                    return false;
                }
            }
            else {
                LOG_ERROR << "rename Failed: " << e.what();
                return false;
            }
        }
        catch(const std::exception& e) {
            LOG_ERROR << "Unexpected error: " << e.what();
            return false;
        }
        return true;
    }


    // 获取指定的文件块（复用打开的文件句柄，避免每块重复 open/close）
    bool readChunk(const std::string& file_id, uint64_t offset, uint32_t size, std::string& data) {
        if (size == 0) {  // 读0字节逻辑上算成功
            data.clear();
            return true;
        }

        std::lock_guard<std::mutex> lock(streams_mutex_);

        auto it = read_streams_.find(file_id);
        if (it == read_streams_.end()) {
            std::string path = getFilePath(file_id);
            std::ifstream fs(path, std::ios::binary);
            if (!fs) {
                LOG_ERROR << "File not found: " << path;
                return false;
            }
            it = read_streams_.emplace(file_id, std::move(fs)).first;
        }

        std::ifstream& file = it->second;

        // 清除可能残留的 eof 状态位，便于断点续传时再次 seek
        file.clear();
        file.seekg(offset);
        if (!file) {
            LOG_ERROR << "Seek to offset " << offset << " failed";
            return false;
        }

        data.resize(size);
        file.read(&data[0], size);

        std::streamsize bytes_read = file.gcount();
        data.resize(static_cast<size_t>(bytes_read));

        if (file.bad()) {
            LOG_ERROR << "I/O error while reading file: " << getFilePath(file_id);
            return false;
        }

        if (bytes_read == 0) {
            // 到达文件末尾，返回空数据
            return true;
        }

        return true;
    }

    // 关闭下载读取句柄
    void closeReadFile(const std::string& file_id) {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = read_streams_.find(file_id);
        if (it != read_streams_.end()) {
            it->second.close();
            read_streams_.erase(it);
        }
    }

    // 硬链接文件(用于去重)：磁盘上只保留一份物理文件，多个文件ID共享同一inode
    bool linkFile(const std::string& src_file_id, const std::string& dst_file_id) {
        std::string src_path = getFilePath(src_file_id); 
        std::string dst_path = getFilePath(dst_file_id); 

        if(!std::filesystem::exists(src_path)) {
            LOG_ERROR << "Source file not found: " << src_path;
            return false;
        }

        std::error_code ec;
        std::filesystem::create_hard_link(src_path, dst_path, ec);
        if(!ec) {
            LOG_INFO << "file linked: " << src_path << " -> " << dst_path;
            return true;
        }

        // 硬链接失败(如跨文件系统)，退化为复制
        try {
            std::filesystem::copy_file(src_path, dst_path, std::filesystem::copy_options::overwrite_existing, ec);
            if(!ec) {
                LOG_INFO << "file copied (fallback): " << src_path << " -> " << dst_path;
                return true;
            }
        }
        catch(const std::exception& e) {
            LOG_ERROR << "Failed to copy file: " << e.what();
            return false;
        }
        return false;
    } 

    // 获取已上传文件的大小
    uint64_t getFileSize(const std::string& file_id) {
        std::string path = getFilePath(file_id);
        if(!std::filesystem::exists(path)) {
            return 0;
        }

        return std::filesystem::file_size(path);
    }

    // 删除文件
    bool deleteFile(const std::string& file_id, bool delete_db = true) {
        {
            std::lock_guard<std::mutex> lock(streams_mutex_);
            auto t = temp_streams_.find(file_id);
            if (t != temp_streams_.end()) {
                t->second.close();
                temp_streams_.erase(t);
            }
            auto r = read_streams_.find(file_id);
            if (r != read_streams_.end()) {
                r->second.close();
                read_streams_.erase(r);
            }
        }

        std::string path = getFilePath(file_id);
        if(std::filesystem::exists(path)) {
            std::filesystem::remove(path);
        }

        std::string temp_path = getTempPath(file_id);
        if(std::filesystem::exists(temp_path)) {
            std::filesystem::remove(temp_path);
        }

        if(delete_db) {
            FileDAO dao;
            dao.deleteFileRecord(file_id);
        }

        LOG_INFO << "File deleted: " << file_id;
        return true;
    }


    // 定时清理文件
    void cleanExpiredFiles() {
        FileDAO dao;
        int64_t now = tool::getTimestamp();

        auto expired = dao.getExpiredFile(now);
        for(const auto& rec : expired) {
            dao.deleteOfflineFileRecord(rec.file_id, rec.to_uid);
            deleteFile(rec.file_id, true); // 删除文件
            LOG_INFO << "Cleaned expired file: " << rec.file_id;
        }

        for(const auto& entry : std::filesystem::directory_iterator(temp_dir_)) {
            if(entry.is_regular_file()) {
                auto last_write = std::filesystem::last_write_time(entry);
                // auto now_time = std::chrono::system_clock::now();
                auto now_time = decltype(last_write)::clock::now();
                if(now_time - last_write > std::chrono::hours(24)) {
                    std::filesystem::remove(entry.path());
                    LOG_INFO << "Cleaned orphan temp file: " << entry.path().filename();
                }
            }
        }
    }

    // 线程在后台自动清理
    void startClean() {
        // 防止重复启动
        if(cleaner_running_) {
            return ;
        }

        // 设置后台运行
        cleaner_running_ = true;
        cleaner_thread_ = std::thread([this]() {
            std::unique_lock<std::mutex> lock(cleaner_mutex_);
            while(cleaner_running_) {
                // 可被 stopCleaner 唤醒，避免关闭时 join 阻塞长达 1 小时
                cleaner_cv_.wait_for(lock, std::chrono::hours(1), [this]() {
                    return !cleaner_running_;
                });
                if(!cleaner_running_) {
                    break;
                }
                lock.unlock();
                cleanExpiredFiles();  // 执行清理文件
                lock.lock();
            }
        });
        LOG_INFO << "File cleaner start";
    }

    // 停止后台线程
    void stopCleaner() {
        {
            std::lock_guard<std::mutex> lock(cleaner_mutex_);
            cleaner_running_ = false;
        }
        cleaner_cv_.notify_all();
        if(cleaner_thread_.joinable()) {
            cleaner_thread_.join();
        }
    }
    
private:
    std::string base_dir_;
    std::string store_dir_;
    std::string temp_dir_;
    int expired_days_;
    std::atomic<bool> cleaner_running_{false};
    std::thread cleaner_thread_;
    std::mutex cleaner_mutex_;
    std::condition_variable cleaner_cv_;

    // 复用已打开的文件句柄，避免每块重复 open/close
    std::mutex streams_mutex_;
    std::unordered_map<std::string, std::fstream> temp_streams_;
    std::unordered_map<std::string, std::ifstream> read_streams_;
};