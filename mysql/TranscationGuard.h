#pragma once

// 事务(一致性)

#include "baseDAO.h"
#include <stdexcept>

class TransactionGuard{
public:
    // 构造函数：传入 DAO 对象，自动开启事务
    explicit TransactionGuard(BaseDAO& dao) 
        : dao_(dao), committed_(false) {
        if (!dao_.inTransaction()) {
            if (dao_.beginTransaction()) {
                started_ = true;
                LOG_DEBUG << "Transaction started";
            } else {
                LOG_ERROR << "Failed to begin transaction";
                throw std::runtime_error("Failed to begin transaction");
            }
        } else {
            LOG_DEBUG << "Already in transaction, reusing existing";
            started_ = false;
        }
    }
    
    // 析构函数：如果是事务发起者且未提交，自动回滚
    ~TransactionGuard() {
        try {
            if (started_ && !committed_) {
                dao_.rollbackTransaction();
            }
        } catch (...) {
            // 析构函数不能抛出异常，静默处理
        }
    }
    
    // 手动提交事务（嵌套作用域中为空操作，由外层发起者统一提交）
    void commit() {
        if (!started_) {
            LOG_DEBUG << "Commit skipped: nested transaction scope";
            return;
        }
        if (!committed_) {
            if (!dao_.commitTransaction()) {
                throw std::runtime_error("Failed to commit transaction");
            }
            committed_ = true;
            LOG_DEBUG << "Transaction committed";
        }
    }
    
    // 手动回滚事务（嵌套作用域中为空操作）
    void rollback() {
        if (!started_) {
            return;
        }
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
    bool started_;
};