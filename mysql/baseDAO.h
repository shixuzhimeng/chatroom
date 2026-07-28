#pragma once

#include "mysqlPool.h"
#include "../epoll.h"
#include "../logging.h"
#include <string>
#include <vector>
#include <map>
#include <functional>

class BaseDAO {
public:
    virtual ~BaseDAO() = default;

private:
    // 查询操作
    bool executeQuery(const std::string& sql, std::vector<std::map<std::string, std::string>>& result) {
        putbackConnection conn;
        if(!conn.connect()) {
            LOG_ERROR << "Database connection failed";
            return false;
        }
        
        // 查询
        if(mysql_query(conn.get(), sql.c_str())) {
            LOG_ERROR << "Query failed: " << mysql_errno(conn.get()) << "SQL:" << sql;
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
        putbackConnection conn;
        if(!conn.connect()) {
            LOG_ERROR << "Database connection failed";
            return false;
        }
        if(mysql_query(conn.get(), sql.c_str())) {
            LOG_ERROR << "Update failed:" << mysql_errno(conn.get()) << "SQL:" << sql;
            return false;
        }
        return false;
    }

    // 获取新插入的ID
    uint64_t getLastInserterID() {
        putbackConnection conn;
        if(!conn.connect()) {
            LOG_ERROR << "Database connection failed";
            return false;
        }

        return mysql_insert_id(conn.get());
    }

    // 防止SQL注入
    std::string escaped(const std::string& str) {
        putbackConnection conn;
        if(!conn.connect()) {
            return str;
        }

        char* escaped = new char[str.length()* 2 + 1];
        mysql_real_escape_string(conn.get(), escaped, str.c_str(), str.length());
        std::string result(escaped);
        delete[] escaped;
        return result;
    }

    // 将数据转换为SQL语句
    std::string bulidSentence(const std::map<std::string, std::string>& conditions) {
        
    }
};