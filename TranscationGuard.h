#pragma once

// 事务(一致性)

#include "baseDAO.h"
#include <stdexcept>

class TransactionGuard{
public:
    // 构造函数：传入 DAO 对象，自动开启事务
    explicit TransactionGuard(BaseDAO& dao) 
        : dao_(dao), committed_(false) {
        if (!dao_.beginTransaction()) {
            throw std::runtime_error("Failed to begin transaction");
        }
    }
    
    // 析构函数：如果没有提交，自动回滚
    ~TransactionGuard() {
        try {
            if (!committed_) {
                dao_.rollbackTransaction();
            }
        } catch (...) {
            // 析构函数不能抛出异常，静默处理
        }
    }
    
    // 手动提交事务
    void commit() {
        if (!committed_) {
            if (!dao_.commitTransaction()) {
                throw std::runtime_error("Failed to commit transaction");
            }
            committed_ = true;
        }
    }
    
    // 手动回滚事务
    void rollback() {
        if (!committed_) {
            if (!dao_.rollbackTransaction()) {
                throw std::runtime_error("Failed to rollback transaction");
            }
            committed_ = true;
        }
    }
    
    // 检查事务是否已提交
    bool isCommitted() const { return committed_; }
    
    // 禁止拷贝和赋值（事务对象不可复制）
    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;
    
private:
    BaseDAO& dao_;
    bool committed_;
};