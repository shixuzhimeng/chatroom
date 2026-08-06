#pragma once

#include "baseDAO.h"
#include "tool.h"

class BlockDAO : public BaseDAO{
public:
    // 屏蔽用户
    bool blockUser(uint64_t user_id, uint64_t block_id) {
        if(isBlocked(user_id, block_id)) {
            return true;
        }
        uint64_t now = tool::getTimestamp();
        std::string sql = "INSERT INTO block_list (user_id, block_id, created_at, updated_at)"
                          "VALUES (" + std::to_string(user_id) + ", " + std::to_string(block_id) + ", " + 
                          std::to_string(now) + ", " + std::to_string(now) + ")";
        return executeUpdate(sql);
    }

    // 取消屏蔽
    bool unblockUser(uint64_t user_id, uint64_t block_id) {
        std::string sql = "DELETE FROM block_list WHERE user_id = " + std::to_string(user_id) + 
        " AND block_id = " + std::to_string(block_id);
        executeUpdate(sql);
        return true;
    }


    // 检查是否屏蔽
    bool isBlocked(uint64_t user_id, uint64_t target_id) {
        std::string sql = "SELECT 1 FROM block_list WHERE user_id = " + std::to_string(user_id) + 
        " AND block_id = " + std::to_string(target_id) + " LIMIT 1";
        std::vector<std::map<std::string, std::string>> result;

        executeQuery(sql, result);
        return !result.empty();
    }

    // 获取屏蔽列表
    std::vector<uint64_t> getBlockList(uint64_t user_id) {
        std::vector<uint64_t> list;
        std::string sql = "SELECT block_id FROM block_list WHERE user_id = " + std::to_string(user_id);
        std::vector<std::map<std::string, std::string>> result;
        if(!executeQuery(sql, result)) {
            return list;
        }
        for(const auto& row : result){
            list.push_back(std::stoull(row.at("block_id")));
        }
        return list;
    }
};