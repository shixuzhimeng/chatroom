// 用于网络信息数据的传输的时候序列化和反序列化
#pragma once

#include "p.pb.h"
#include "../logging.h"
#include "../epoll.h"
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <google/protobuf/util/json_util.h>
#include <algorithm>

using std::vector;
namespace proto{

class MessageCodec{
public:
    // 编码
    static std::vector<char> encode(const p::MessageHeader& header, const google::protobuf::Message& body) {
        std::string body_str;
        if(!body.SerializeToString(&body_str)) {
            LOG_ERROR << "Failed to Serialize body";
            return {};
        }

        p::MessageHeader h = header;
        h.set_body_length(body_str.size());

        std::string header_str;
        if(!h.SerializeToString(&header_str)) {
            LOG_ERROR << "Failed to Serialize header";
            return {};
        }
        
        std::vector<char> result;
        uint32_t header_len = header_str.size();
        uint32_t body_len = body_str.size();
        uint32_t total_len = header_len + body_len;
        uint32_t net_total_len = htonl(total_len);
        uint32_t net_header_len = htonl(header_len);
        
        result.resize(sizeof(net_total_len) + sizeof(net_header_len) + total_len);
        
        size_t pos = 0;
        memcpy(result.data() + pos, &net_total_len, sizeof(net_total_len));
        pos += sizeof(net_total_len);
        memcpy(result.data() + pos, &net_header_len, sizeof(net_header_len));
        pos += sizeof(net_header_len);
        memcpy(result.data() + pos, header_str.data(), header_len);
        pos += header_len;
        memcpy(result.data() + pos, body_str.data(), body_len);

        LOG_INFO << "encode: type=" << header.msg_type() 
                << ", total_len=" << total_len 
                << ", header_len=" << header_len
                << ", body_len=" << body_len;

        return result;
    }

    // 解码
    static bool decode(const std::vector<char>& data, size_t& consumed, p::MessageHeader& header, std::vector<char>& body){
        LOG_INFO << "decode: ENTER, data.size()=" << data.size();

        consumed = 0;
        body.clear();
        header.Clear();

        constexpr size_t PREFIX_LEN = sizeof(uint32_t) * 2;


        if (data.size() < PREFIX_LEN) {
            LOG_INFO << "decode: incomplete prefix, need="
                    << PREFIX_LEN
                    << ", have=" << data.size();
            return false;
        }

        uint32_t total_len = 0;
        uint32_t header_len = 0;

        memcpy(&total_len,
            data.data(),
            sizeof(uint32_t));

        memcpy(&header_len,
            data.data() + sizeof(uint32_t),
            sizeof(uint32_t));

        total_len = ntohl(total_len);
        header_len = ntohl(header_len);

        LOG_INFO << "decode: total_len=" << total_len
                << ", header_len=" << header_len;

        // 防止整数/长度异常
        if (total_len < header_len) {
            LOG_ERROR << "decode FAILED: total_len < header_len";
            return false;
        }

        // 一个完整包的总长度：
        // 4 + 4 + total_len
        const size_t packet_len = PREFIX_LEN + total_len;

        if (data.size() < packet_len) {
            LOG_INFO << "decode: incomplete packet, need="
                    << packet_len
                    << ", have=" << data.size();
            return false;
        }

        size_t pos = PREFIX_LEN;

        if (!header.ParseFromArray(
                data.data() + pos,
                header_len))
        {
            LOG_ERROR << "decode: header ParseFromArray failed";
            return false;
        }

        LOG_INFO << "decode: header parsed"
                << ", msg_type=" << header.msg_type()
                << ", body_length=" << header.body_length();

        pos += header_len;


        const size_t body_len = total_len - header_len;

        if (header.body_length() != body_len) {
            LOG_ERROR << "decode FAILED: body length mismatch, "
                    << "header.body_length()=" << header.body_length()
                    << ", actual=" << body_len;
            return false;
        }

        if (body_len > 0) {
            body.assign(
                data.data() + pos,
                data.data() + pos + body_len
            );
        }

        consumed = packet_len;

        LOG_INFO << "decode: SUCCESS, consumed="
                << consumed;

        return true;
    }
};

class Dispatch{
public:
    using handle = std::function<void(std::shared_ptr<TcpConnection>, const p::MessageHeader&, const std::vector<char>&)>;

    void registerHandle(p::MessageType type, handle handle) {
        handles[type] = handle;
        LOG_INFO << "registerHandle for type :" << type;
    }

    void Dispatcher(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        LOG_INFO << "Dispatcher: msg_type=" << header.msg_type();  // ← 添加这行
        
        auto it = handles.find(header.msg_type());

        if(it != handles.end()) {
            LOG_INFO << "Dispatcher: found handler for type " << header.msg_type();  // ← 添加这行
            try {
                it->second(conn, header, body);
                LOG_INFO << "Dispatcher: handler executed successfully";  // ← 添加这行
            }
            catch(const std::exception& e) {
                LOG_ERROR << "Handle exception: " << e.what();
            }
        }
        else {
            LOG_INFO << "No handle for this type: " << header.msg_type();
        }
    }

    size_t handlesCount() const {
        return handles.size();
    }

private:
    std::unordered_map<p::MessageType, handle> handles;
};

// json和protobuf相互转换
class PS{
public:
    template<typename T>
    static std::string toJ(const T& message) {
        std::string json_str;
        google::protobuf::util::MessageToJsonString(message, &json_str);
        return json_str;
    }

    template<typename T>
    static bool fromJson(const std::string& json, T& message) {
        auto status = google::protobuf::util::JsonStringToMessage(json, &message);
        return status.ok();
    }
};

}