#pragma once

#include <openssl/sha.h>
#include <openssl/rand.h>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>


class Crypot {
public:

    // 生成随机盐值
    static std::string generateSalt(size_t length = 16) {
        std::vector<unsigned char> salt(length);
        if(RAND_bytes(salt.data(), length)) {
            throw std::runtime_error("failed to generate salt");
        }
        return bytesToHex(salt.data(),length);
    }

    // 计算SHA-256哈希
    static std::string sha256(const std::string& input) {
        unsigned char hash[SHA256_DIGEST_LENGTH]; // 常量，通常32字节
        SHA256_CTX sha256;
        SHA256_Init(&sha256);
        SHA256_Update(&sha256, input.c_str(), input.size());
        SHA256_Final(hash, &sha256);   //将计算完的结果存储在hash中
        return bytesToHex(hash, SHA256_DIGEST_LENGTH);
    }

    // 密码加密
    static std::string encryptPassword(const std::string& password, const std::string& salt) {
        return sha256(password + salt);
    }

    // 验证密码
    static bool verifyPassword(const std::string& password, const std::string& salt, const std::string& hash)    {
        return encryptPassword(password, salt) == hash;
    }

    static std::string generateID(size_t length = 32) {
        std::vector<unsigned char> id(length);
        if(RAND_bytes(id.data(), length) != 1) {
            throw std::runtime_error("Failed to generate random id");
        }
        return bytesToHex(id.data(), length);
    }

    // 验证码
    static std::string generateYanZengCode() {
        std::string code;
        code.resize(6);
        for(int i = 0; i < 6; i++) {
            code[i] = '0' + (rand() % 10);
        }
        return code;
    }


private:
    // 二进制字节序转换为十六进制可读字符串
    static std::string bytesToHex(const unsigned char* data, size_t len) {
        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for(size_t i = 0; i < len; i++) {
            ss << std::setw(2) << static_cast<int>(data[i]);
        }
        return ss.str();
    }
};