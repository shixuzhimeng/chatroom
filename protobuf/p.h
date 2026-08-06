// 用于网络信息数据的传输的时候序列化和反序列化
#pragma once

#include "p.pb.h"
#include "../logging.h"
#include "../epoll.h"
#include <vector>
#include <memory>
#include <functional>
#include <vector>
#include <unordered_map>
#include <google/protobuf/util/json_util.h>

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
        uint32_t total_len = header_str.size() + body_str.size();
        result.resize(sizeof(total_len) + total_len);

        memcpy(result.data(), &total_len, sizeof(total_len));
        memcpy(result.data() + sizeof(total_len), header_str.data(), header_str.size());
        memcpy(result.data() + sizeof(total_len) + header_str.size(), body_str.data(), body_str.size());

        LOG_DEBUG << "type : " << header.msg_type() << "len : " << total_len;

        return result;
    }


    // 解码
    static bool decode(vector<char>& data, size_t& consumed, p::MessageHeader& header, vector<char>& body) {
        consumed = 0;
        if (data.size() < sizeof(uint32_t)) return false;
        
        uint32_t total_len;
        memcpy(&total_len, data.data(), sizeof(uint32_t));
        total_len = ntohl(total_len);
        
        if (data.size() < sizeof(uint32_t) + total_len) return false;
        
        size_t pos = sizeof(uint32_t);
        
        // 解析 header
        if (!header.ParseFromArray(data.data() + pos, total_len)) {
            LOG_ERROR << "header parse failed";
            return false;
        }
        
        // 获取 header 实际序列化长度
        size_t header_len = header.ByteSizeLong();
        pos += header_len;
        
        uint32_t body_len = header.body_length();
        if (body_len > 0 && body_len <= total_len - header_len) {
            body.assign(data.data() + pos, data.data() + pos + body_len);
            pos += body_len;
        }
        
        consumed = pos;
        LOG_DEBUG << "type: " << header.msg_type() << ", body_len: " << body_len << ", consumed: " << consumed;
        return true;
    }
};

class Dispatch{
public:
    using handle = std::function<void(std::shared_ptr<TcpConnection>, const p::MessageHeader&, const std::vector<char>&)>;

    void registerHandle(p::MessageType type, handle handle) {
        handles[type] = handle;
        LOG_INFO << "registerHandle for type :" << type << std::endl;
    }

    void Dispatcher(std::shared_ptr<TcpConnection> conn, const p::MessageHeader& header, const std::vector<char>& body) {
        auto it = handles.find(header.msg_type());

        if(it != handles.end()) {
            try {
                it->second(conn, header, body);
            }
            catch(const std::exception& e) {
                LOG_ERROR << "Handle exception" << e.what() << std::endl;
            }
        }
        else {
            LOG_INFO << "No handle for this type" << header.msg_type() << std::endl;
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
    // 将消息换为Json类
    template<typename T>
    static std::string toJ(const T& message) {
        std::string json_str;
        google::protobuf::util::MessageToJsonString(message, &json_str);
        return json_str;
    }


    // 将Json类转换为消息
    template<typename T>
    static bool fromJson(const std::string& json, T& message) {
        auto status = google::protobuf::util::JsonStringToMessage(json, &message);
  
        return status.ok();
    }
};

}