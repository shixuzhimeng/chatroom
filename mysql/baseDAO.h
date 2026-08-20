#pragma once

#include "mysqlPool.h"
#include "../epoll.h"
#include "../logging.h"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include "protobuf/mysql_p.h"
#include <nlohmann/json.hpp>
#include "../Check.h"

class BaseDAO {
public:
    virtual ~BaseDAO() = default;

    // 事务
    bool beginTransaction() {
        return executeUpdate("START TRANSACTION");
    }
    
    bool commitTransaction() {
        return executeUpdate("COMMIT");
    }
    
    bool rollbackTransaction() {
        return executeUpdate("ROLLBACK");
    }

    bool inTransaction() const {
        return in_transaction_;
    }

    void setConnection(MYSQL* conn) {
        conn_ = conn;
    }




    // 查询操作
    bool executeQuery(const std::string& sql, std::vector<std::map<std::string, std::string>>& result) {
        putbackConnection conn;
        if(!conn.connect()) {
            LOG_ERROR << "Database connection failed";
            return false;
        }
        
        // 查询
        if(mysql_query(conn.get(), sql.c_str())) {
            LOG_ERROR << "Query failed: " << mysql_error(conn.get()) << "SQL:" << sql;
            return false;
        }
        
        // 结果集
        MYSQL_RES* res = mysql_store_result(conn.get());
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
        
        putbackConnection conn;
        if(!conn.connect()) {
            LOG_ERROR << "Database connection failed";
            return false;
        }
        if(mysql_query(conn.get(), sql.c_str())) {
            LOG_ERROR << "Update failed:" << mysql_errno(conn.get()) << "SQL:" << sql;
            return false;
        }
        
        // 如果是 INSERT 语句，保存最后插入的 ID
        if (sql.find("INSERT") == 0) {
            last_insert_id_ = mysql_insert_id(conn.get());
            LOG_INFO << "executeUpdate: inserted ID = " << last_insert_id_;
        }
        
        return true;
    }
protected:
    // 获取新插入的ID
    uint64_t getLastInsertID() {
        return last_insert_id_;
    }

    // 防止SQL注入
    std::string escapeString(const std::string& str) {
        putbackConnection conn;
        if(!conn.connect()) {
            return str;
        }
        std::vector<char> escaped(str.length() * 2 + 1);
        mysql_real_escape_string(conn.get(), escaped.data(), str.c_str(), str.length());
        return std::string(escaped.data());
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
};