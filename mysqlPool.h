#include <mysql/mysql.h>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <chrono>
#include <stdexcept>
#include "logging.h"


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
private:
    MYSQL* conn_;
};