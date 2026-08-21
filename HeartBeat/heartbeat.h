#pragma once

#include "protobuf/p.h"
#include "../logging.h"
#include "../tool.h"
#include "../epoll.h"
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include "friend/OnlineManager.h"

class HeartbeatHandle {
public:
    HeartbeatHandle() = default;

    void setUserConnections(std::unordered_map<uint64_t, std::shared_ptr<TcpConnection>>* conn) {
        user_connections_ = conn;
    }

    // 处理心跳请求
    void handleHeartBeat(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        p::Heartbeat req;
        if(!req.ParseFromArray(body.data(), body.size())) {
            LOG_ERROR << "Parse Heartbeat failed";
            return ;
        }

        uint64_t user_id = conn->getUserID();
        uint32_t seq = req.seq();
        if(user_id == 0) {
            sendHeartbeatResp(conn, header, req.seq(), 0);
            return ;
        }

        if(seq == 0) {
            sendHeartbeatResp(conn, header, seq, 0);
            return ;
        }

        // 去重检查
        std::string key = std::to_string(user_id) + "_" + std::to_string(req.seq());

        {
            std::lock_guard<std::mutex> lock(seq_mutex_);
            if(processed_seqs_.find(key) != processed_seqs_.end()) {
                LOG_DEBUG << "Duplicate heartbeat seq: " << req.seq() << " for user " << user_id;
                return ;
            }
            processed_seqs_.insert(key);

            if(processed_seqs_.size() > 10000) {
                processed_seqs_.clear();
            }
        }

        // 更新用户心跳时间，防止被超时标记为离线
        OnlineManager::getInstance().updateHeartbeat(user_id);

        // 更新用户的最后的活跃时间
        conn->setContext(reinterpret_cast<void*>(user_id));

        LOG_DEBUG << "Heartbeat from user: " << user_id << ", seq= " << req.seq();

        // 计算延迟
        int64_t now = tool::getTimestamp();
        int64_t latency = now - req.timestamp();

        // 响应
        sendHeartbeatResp(conn, header, req.seq(), latency);
    }
private:
    std::unordered_map<uint64_t, std::shared_ptr<TcpConnection>>* user_connections_ = nullptr;
    std::mutex seq_mutex_;
    std::unordered_set<std::string> processed_seqs_;

    void sendHeartbeatResp(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, uint64_t seq, int64_t latency) {
        p::HeartbeatResp resp;
        resp.set_timestamp(tool::getTimestamp());
        resp.set_server_time(tool::getTimestamp());
        resp.set_seq(seq);
        resp.set_latency_ms(latency);

        p::MessageHeader resp_header;
        resp_header.set_msg_id(header.msg_id() + 1);
        resp_header.set_msg_type(header.msg_type());
        resp_header.set_timestamp(tool::getTimestamp());

        auto data = proto::MessageCodec::encode(resp_header, resp);
        if(!data.empty()) {
            conn->send(data.data(), data.size());
        }
    }
};