#pragma once

#include "mysqlPool.h"
#include "net/epoll.h"
#include "tool/logging.h"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include "protobuf/mysql_p.h"
#include <nlohmann/json.hpp>
#include "tool/Check.h"

class BaseDAO {
public:
    virtual ~BaseDAO() {
        // 防御：DAO 析构时若仍处于事务中，回滚并归还连接
        if (in_transaction_) {
            rollbackTransaction();
        }
    }

    // 事务：事务期间绑定一条专用连接，保证 BEGIN/语句/COMMIT 在同一连接上执行
    bool beginTransaction() {
        if (in_transaction_) {
            return true;
        }
        tx_conn_ = ConnectionPool::getInstance().getConnection();
        if (!tx_conn_ || !tx_conn_->get()) {
            LOG_ERROR << "beginTransaction: failed to acquire connection";
            return false;
        }
        conn_ = tx_conn_->get();
        if (mysql_query(conn_, "START TRANSACTION") != 0) {
            LOG_ERROR << "START TRANSACTION failed: " << mysql_error(conn_);
            endTransaction();
            return false;
        }
        in_transaction_ = true;
        return true;
    }

    bool commitTransaction() {
        if (!in_transaction_ || !conn_) {
            LOG_ERROR << "commitTransaction: not in transaction";
            return false;
        }
        bool ok = (mysql_query(conn_, "COMMIT") == 0);
        if (!ok) {
            LOG_ERROR << "COMMIT failed: " << mysql_error(conn_);
        }
        endTransaction();
        return ok;
    }

    bool rollbackTransaction() {
        if (!in_transaction_ || !conn_) {
            return false;
        }
        bool ok = (mysql_query(conn_, "ROLLBACK") == 0);
        if (!ok) {
            LOG_ERROR << "ROLLBACK failed: " << mysql_error(conn_);
        }
        endTransaction();
        return ok;
    }

    bool inTransaction() const {
        return in_transaction_;
    }

    void setConnection(MYSQL* conn) {
        conn_ = conn;
    }




    // 查询操作
    bool executeQuery(const std::string& sql, std::vector<std::map<std::string, std::string>>& result) {
        // 事务中复用绑定的连接，否则临时从池中获取
        MYSQL* c = conn_;
        putbackConnection pc;
        if (!c) {
            if(!pc.connect()) {
                LOG_ERROR << "Database connection failed";
                return false;
            }
            c = pc.get();
        }

        // 查询
        if(mysql_query(c, sql.c_str())) {
            LOG_ERROR << "Query failed: " << mysql_error(c) << "SQL:" << sql;
            return false;
        }

        // 结果集
        MYSQL_RES* res = mysql_store_result(c);
        if(!res) {
            LOG_ERROR << "Store result failed";
            return false;
        }
        // 获取字段数量
        int num = mysql_num_fields(res);
        MYSQL_ROW row;

        // 遍历
        while((row = mysql_fetch_row(res))) {
            std::map<std::string, std::string> r;
            for(int i = 0; i < num; i++) {
                MYSQL_FIELD* field = mysql_fetch_field_direct(res, i);
                r[field->name] = row[i] ? row[i] : "";
            }
            result.push_back(r);
        }

        mysql_free_result(res);

        return true;
    }

    // 更新数据（增，删，改）
    bool executeUpdate(const std::string& sql) {
        LOG_DEBUG << "start executeUpdate";
        LOG_DEBUG << "SQL: " << sql;

        MYSQL* c = conn_;
        putbackConnection pc;
        if (!c) {
            if(!pc.connect()) {
                LOG_ERROR << "Database connection failed";
                return false;
            }
            c = pc.get();
        }

        if(mysql_query(c, sql.c_str())) {
            LOG_ERROR << "Update failed:" << mysql_errno(c) << "SQL:" << sql;
            return false;
        }

        // 如果是 INSERT 语句，保存最后插入的 ID
        if (sql.find("INSERT") == 0) {
            last_insert_id_ = mysql_insert_id(c);
            LOG_INFO << "executeUpdate: inserted ID = " << last_insert_id_;
        }

        return true;
    }
protected:
    // 获取新插入的ID
    uint64_t getLastInsertID() {
        return last_insert_id_;
    }

    // 结束事务并归还绑定的连接
    void endTransaction() {
        if (tx_conn_) {
            tx_conn_.reset(); // deleter 会将连接归还连接池
        }
        conn_ = nullptr;
        in_transaction_ = false;
    }

    // 防止SQL注入
    std::string escapeString(const std::string& str) {
        MYSQL* c = conn_;
        putbackConnection pc;
        if (!c) {
            if(!pc.connect()) {
                LOG_ERROR << "escapeString: no connection, using basic escape";
                return basicEscape(str);
            }
            c = pc.get();
        }
        std::vector<char> escaped(str.length() * 2 + 1);
        unsigned long n = mysql_real_escape_string(c, escaped.data(), str.c_str(), str.length());
        return std::string(escaped.data(), n);
    }

    static std::string basicEscape(const std::string& str) {
        std::string out;
        out.reserve(str.size() * 2);
        for (unsigned char ch : str) {
            switch (ch) {
                case 0:
                    continue; // 无法经此路径存储，直接丢弃
                case '\\': out += "\\\\"; break;
                case '\'': out += "\\'"; break;
                case '"':  out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case 26:   out += "\\Z"; break;
                default:   out += static_cast<char>(ch); break;
            }
        }
        return out;
    }
    // 将数据转换为SQL语句
    std::string bulidSentence(const std::map<std::string, std::string>& conditions) {
        if(conditions.empty()) {
            return "";
        }
        std::string where = " WHERE ";
        bool first = true;
        for (const auto& pair : conditions) {
            const auto& key = pair.first;
            const auto& value = pair.second;
            if(!first) {
                where += " AND ";
            }
            where += key + " = '" + escapeString(value) + "'";
            first = false;
        }
        return where;
    }

    

    template<typename T>
    std::string sExtra(const T& message) {
        return Switch::sToJson(message);
    }
    
    template<typename T>
    bool dsExtra(const std::string& json_str, T& message) {
        return Switch::dsFromJson(json_str, message);
    }

    // 将字段值转换为JSON
    std::string toJsonString(const std::map<std::string, std::string>& data) {
        std::string json = "{";
        bool first = true;
        for (const auto& [key, value] : data) {
            if (!first) json += ",";
            json += "\"" + key + "\":\"" + escapeString(value) + "\"";
            first = false;
        }
        json += "}";
        return json;
    }
    
    // 从JSON解析字段值
    std::map<std::string, std::string> fromJsonString(const std::string& json_str) {
        std::map<std::string, std::string> result;
        // 简单解析（实际应使用JSON库）
        if (json_str.empty() || json_str == "{}") {
            return result;
        }
        
        // 使用nlohmann/json解析
        try {
            auto json = nlohmann::json::parse(json_str);
            for (auto& [key, value] : json.items()) {
                if (value.is_string()) {
                    result[key] = value.get<std::string>();
                } else {
                    result[key] = value.dump();
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR << "Parse JSON failed: " << e.what();
        }
        return result;
    }

    std::string safeSqlValidator(const std::string& input) {
        if(InputValidator::hasSQLInjectionRisk(input)) {
            LOG_ERROR << "SQL injection risk delected in input";
            return "";
        }

        return escapeString(input);
    }

    MYSQL* conn_ = nullptr;
    uint64_t last_insert_id_ = 0;
    bool in_transaction_ = false;
    // 事务期间绑定的专用连接
    std::unique_ptr<MySQLConnection, std::function<void(MySQLConnection*)>> tx_conn_;
};