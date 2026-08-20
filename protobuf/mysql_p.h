// 数据库的数据序列化和反序列化传输
#pragma once

#include "mysql.pb.h"
#include "../logging.h"
#include <string>
#include <vector>
#include <memory>
#include <google/protobuf/util/json_util.h>

class Switch {
public:

    // protobuf和json相互转换
    template<typename T>
    static std::string sToJson(const T& message) {
        std::string json_str;
        google::protobuf::util::JsonOptions options;
        options.add_whitespace = false; // 选择无空格
        options.always_print_primitive_fields = true;  // 输出基础类型字段（字段值为默认值也会输出）
        
        auto status = google::protobuf::util::MessageToJsonString(message, &json_str, options); // 转换
        if(!status.ok()) {
            LOG_ERROR << "Switch to JSON failed: " << status.ToString();
            return "{}";
        }
        return json_str;
    }
    
    template<typename T>
    static bool dsFromJson(const std::string& json_str, T& message) {
        if(json_str.empty() || json_str == "{}") {
            return false;
        }
        
        google::protobuf::util::JsonParseOptions options;
        options.ignore_unknown_fields = true;
        
        auto status = google::protobuf::util::JsonStringToMessage(json_str, &message, options);
        if(!status.ok()) {
            LOG_WARN << "Deserialize from JSON failed: " << status.ToString();
            return false;
        }
        return true;
    }
    
    // p转化为二进制    
    template<typename T>
    static std::string sTo2(const T& message) {
        std::string binary_str;
        if(!message.SerializeToString(&binary_str)) {
            LOG_ERROR << "Switch to 2 failed";
            return "";
        }
        return binary_str;
    }
    
    // 二进制转换为p
    template<typename T>
    static bool dsFrom2(const std::string& binary_str, T& message) {
        if(binary_str.empty()) {
            return false;
        }
        return message.ParseFromString(binary_str);
    }
    
    static std::string encodeBase(const std::string& data) {
        static const char* base64_chars = 
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        
        std::string result;
        int i = 0;
        unsigned char char_array_3[3];
        unsigned char char_array_4[4];
        
        for(size_t idx = 0; idx < data.length(); idx++) {
            char_array_3[i++] = data[idx];
            if(i == 3) {
                char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                char_array_4[3] = char_array_3[2] & 0x3f;
                
                for (i = 0; i < 4; i++) {
                    result += base64_chars[char_array_4[i]];
                }
                i = 0;
            }
        }
        
        if(i) {
            for(int j = i; j < 3; j++) {
                char_array_3[j] = '\0';
            }
            
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            
            for(int j = 0; j < i + 1; j++) {
                result += base64_chars[char_array_4[j]];
            }
            
            while(i++ < 3) {
                result += '=';
            }
        }
        
        return result;
    }
    
    static std::string decodeBase64(const std::string& encoded) {
        static const std::string base64_chars = 
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        
        std::string result;
        int i = 0;
        unsigned char char_array_4[4], char_array_3[3];
        
        for (char c : encoded) {
            if(c == '=') break;
            
            size_t pos = base64_chars.find(c);
            if(pos == std::string::npos) continue;
            
            char_array_4[i++] = pos;
            if(i == 4) {
                char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
                char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
                char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
                
                for(int j = 0; j < 3; j++) {
                    result += char_array_3[j];
                }
                i = 0;
            }
        }
        
        if(i) {
            for(int j = i; j < 4; j++) {
                char_array_4[j] = 0;
            }
            
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
            
            for(int j = 0; j < i - 1; j++) {
                result += char_array_3[j];
            }
        }
        
        return result;
    }
};


#define SERIALIZE_TO_JSON(msg) Switch::sToJson(msg)
#define DESERIALIZE_FROM_JSON(json, msg) Switch::dsFromJson(json, msg)
#define SERIALIZE_TO_BINARY(msg) Switch::sTo2(msg)
#define DESERIALIZE_FROM_BINARY(data, msg) Switch::dsFrom2(data, msg)