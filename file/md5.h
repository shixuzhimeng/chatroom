#pragma once

#include <openssl/md5.h>
#include <fstream>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>

class MD5Tool {
public:
    // 计算字符串MD5的哈希值
    static std::string calculate(const std::string& data) {
        unsigned char digest[MD5_DIGEST_LENGTH];
        MD5(reinterpret_cast<const unsigned char*>(data.c_str()), data.size(), digest);
        return ToHexString(digest, MD5_DIGEST_LENGTH);
    }

    // 计算文件的MD5的哈希之值
    static std::string calculateFile(const std::string& file_path) {
        std::ifstream file(file_path, std::ios::binary); // 以二进制的形式打开文件读取
        if(!file) {
            return "";
        }

        // 创建MD5的上下文
        MD5_CTX md5Context;
        MD5_Init(&md5Context);  // 初始化

        char buffer[8192];
        while(file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
            MD5_Update(&md5Context, buffer, file.gcount());
        }

        unsigned char digest[MD5_DIGEST_LENGTH]; // 存储最终的摘要
        MD5_Final(digest, &md5Context);
        return ToHexString(digest, MD5_DIGEST_LENGTH);
    }

    // 校验内容
    static bool verify(const std::string& data, const std::string& expected_md5) {
        return calculate(data) == expected_md5;
    }

    static bool verifyFile(const std::string& file_path, const std::string& expected_md5) {
        return calculateFile(file_path) == expected_md5;
    }

private:
    // 二进制数据转换为十六进制字符串
    static std::string ToHexString(const unsigned  char* data, size_t len) {
        std::stringstream ss;
        ss << std::hex << std::setfill('0');
    
        for(size_t i = 0; i < len; ++i) {
            ss << std::setw(2) << static_cast<int>(data[i]);
        }

        return ss.str();
    }
};