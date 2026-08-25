#pragma once

#include <mysql/mysql.h>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <chrono>
#include <stdexcept>
#include "tool/logging.h"
#include <utility>
#include <functional>

// 数据库整体初始化
class MySQLConnection {
public:
    MySQLConnection(const std::string& host, uint16_t port, const std::string& user, const std::string& password, const std::string& database) {
        
        // 初始化
        conn_ = mysql_init(nullptr);
        if(!conn_) {
            throw std::runtime_error("MySQL init failed");
        }
        bool reconnect = 1;
        mysql_options(conn_, MYSQL_OPT_RECONNECT, &reconnect); // 自动重连
        
        // 连接
        if(!mysql_real_connect(conn_, host.c_str(), user.c_str(), password.c_str(), database.c_str(), port, nullptr, 0)) {
            std::string error = mysql_error(conn_);
            mysql_close(conn_);
            conn_ = nullptr;
            throw std::runtime_error("MySQL connect failed");
        }

        // 设置字符集
        mysql_set_character_set(conn_, "utf8mb4");
    }

    ~MySQLConnection() {
        if(conn_) {
            mysql_close(conn_);
        }
    }

    // 访问器(获取连接的句柄)
    MYSQL* get() { return conn_; }
    MYSQL* operator->() { return conn_; }

    // 移动语句
    // 禁止拷贝
    MySQLConnection(const MySQLConnection&) = delete;
    MySQLConnection& operator=(const MySQLConnection&) = delete;

    MySQLConnection(MySQLConnection&& other) noexcept : conn_(std::exchange(other.conn_, nullptr)) {};
    MySQLConnection& operator=(MySQLConnection&& other) noexcept{
        if(this != &other) {
            if(conn_) {
                mysql_close(conn_);
            }
            conn_ = std::exchange(other.conn_, nullptr);
        }
        return *this;
    }


private:
    MYSQL* conn_;
};

// 连接池创建
class ConnectionPool {
public:
    ConnectionPool() = default;
    static ConnectionPool& getInstance() {
        static ConnectionPool instance;
        return instance;
    }

    bool init(const std::string& host, uint16_t port, const std::string& user, const std::string& password, const std::string& database, size_t min = 5, size_t max = 20, int timeout = 30) {
        // 参数初始化
        std::lock_guard<std::mutex> lock(mutex_);
        host_ = host;
        port_ = port;
        user_ = user;
        password_ = password;
        database_ = database;
        min_ = min;
        max_ = max;
        timeout_ = timeout;
        stop_ = false;
        
        // 创建预连接
        try {
            for(int i = 0; i < min; i++) {
                auto conn = createConnection();

                if(conn) {pool_.push(std::move(conn));};
            }
            LOG_INFO << "Connectionpool init num : " << pool_.size();
            return true;
        }
        catch(const std::exception& e) {
            LOG_ERROR << "Connection failed : " << e.what();
            return false;
        }
    }

    std::unique_ptr<MySQLConnection, std::function<void(MySQLConnection*)>> getConnection() {
        std::unique_lock<std::mutex> lock(mutex_);
        auto start_time = std::chrono::steady_clock::now();

        while(true) {
            if(!pool_.empty()) {
                auto conn = std::move(pool_.front());
                pool_.pop();
            
                if(isConnection(conn.get())) {
                    return std::unique_ptr<MySQLConnection, std::function<void(MySQLConnection*)>>(conn.release(), 
                    [this](MySQLConnection* conn){
                        this->releaseConnection(conn);
                    });
                }
                // 坏连接将被丢弃，同步递减计数，否则 active_count_ 单调增长最终耗尽池容量
                active_count_--;
                try {
                    auto new_conn = createConnection();
                    return std::unique_ptr<MySQLConnection, std::function<void(MySQLConnection*)>>(new_conn.release(),
                        [this](MySQLConnection* conn) {
                            this->releaseConnection(conn);
                        });
                }
                catch(const std::exception& e) {
                    LOG_ERROR << "Create new_conn failed :" << e.what();
                }
                continue;
            }
            size_t cur_size = pool_.size() + active_count_;
            if(cur_size < max_) {
                try {
                    auto conn = createConnection();
                    return std::unique_ptr<MySQLConnection, std::function<void(MySQLConnection*)>>(conn.release(),
                        [this](MySQLConnection* conn) {
                            this->releaseConnection(conn);
                        }); 
                }
                catch(const std::exception& e) {
                    LOG_ERROR << "Create more conn failed :" << e.what();
                }
            }

            auto now = std::chrono::steady_clock::now();
            auto time = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            if(time >= timeout_) {
                throw std::runtime_error("timeout");
            }

            condition_.wait_for(lock, std::chrono::seconds(1));
        }
    }


    ~ConnectionPool() {
        stop_ = true;
        condition_.notify_all();
        while(!pool_.empty()) {
            pool_.pop();
        }
    }

private:
    std::unique_ptr<MySQLConnection> createConnection() {
        auto conn = std::make_unique<MySQLConnection>(host_, port_, user_, password_, database_);
        active_count_++;
        return conn;
    }

    bool isConnection(MySQLConnection* conn) {
        if(!conn || !conn->get()) {
            return false;
        }
        return  mysql_ping(conn->get()) == 0;
    }

    //归还到连接池中
    void releaseConnection(MySQLConnection* conn) {
        if(!conn) {
            return ;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if(stop_) {
            delete conn;
            active_count_--;
            return ;
        }

        if(isConnection(conn)) {
            pool_.push(std::unique_ptr<MySQLConnection>(conn));
        }
        else {
            delete conn;
            active_count_--;
            try {
                auto new_conn = createConnection();
                pool_.push(std::move(new_conn));
            }
            catch(const std::exception& e) {
                LOG_ERROR << "Create new_conn failed" << e.what();
            }
        }

        condition_.notify_one();
    }



    mutable std::mutex mutex_;
    std::string host_;
    uint16_t port_;
    std::string user_;
    std::string password_;
    std::string database_;
    size_t min_;
    size_t max_;
    int timeout_;
    std::atomic<bool> stop_{false};
    std::queue<std::unique_ptr<MySQLConnection>> pool_;
    std::atomic<size_t> active_count_;
    std::condition_variable condition_;
};

// 连接池归还
class putbackConnection {
public:
    putbackConnection() 
    :conn_(nullptr, [](MySQLConnection*){}) {};

    putbackConnection(const putbackConnection&) = delete;
    putbackConnection& operator=(const putbackConnection&) = delete;

    putbackConnection(putbackConnection&& other) = default;
    putbackConnection& operator=(putbackConnection&& other) = default;


    bool connect() { 
        try {
            conn_ = ConnectionPool::getInstance().getConnection();
            return true;
        }
        catch(const std::exception& e) {
            LOG_ERROR << "connect failed : " << e.what();
            return false;
        }
    }

    MYSQL* get() {
        return conn_ ? conn_->get() : nullptr;
    }

    MYSQL* operator->() {
        return get();
    }

    explicit operator bool() const {
        return conn_ != nullptr;
    }

private:
    std::unique_ptr<MySQLConnection, std::function<void(MySQLConnection*)>> conn_;
};