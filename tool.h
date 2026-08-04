#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <random>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <sstream>
#include <climits>

class tool{
public:
    // 去除空格
    static std::string trim(const std::string& str) {
        size_t first = str.find_first_not_of("\t\n\r"); 
        if(first == std::string::npos) {
            return "";
        }
        size_t last = str.find_last_not_of("\t\n\r");
        if(last == std::string::npos) {
            return str.substr(first, last - first + 1);
        }
    }

    // 分割字符串
    static std::vector<std::string> split(const std::string& str, char delimiter) {
        std::vector<std::string> sps;
        std::stringstream ss(str);
        std::string sp;
        while(std::getline(ss, sp, delimiter)) {
            if(!sp.empty()) {
                sps.push_back(sp);
            }
        }
        return sps;
    }

    // 大小写转换
    static std::string toUpper(const std::string& str) {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(), ::toupper);
        return result;
    }

    static std::string toLower(const std::string& str) {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(), ::tolower);
        return result;
    }
    
    // 检查前缀和后缀
    static bool startWith(const std::string& str, const std::string& str2) {
        return str.size() >= str2.size() && str.compare(0, str2.size(), str2) == 0;
    }

    static bool startEnd(const std::string& str, const std::string& str2) {
        return str.size() >= str2.size() && str.compare(str.size() - str2.size(), str2.size(), str2) == 0;
    }

    // 时间
    static std::string getCurTime() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H-%M-%S");
        ss<< "." << std::setfill('0') << std::setw(3) << ms.count();
        return ss.str();
    }

    static std::string getDate() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d");
        return ss.str();
    }

    // 获取时间戳
    static int64_t getTimestamp() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // 随机数生成
    static int random(int min, int max) {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(min, max);
        return dis(gen);
    }

    // 随机字符串的生成
    static std::string randString(int length) {
        static const char s[] = "0123456789"
                                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                "abcdefghijklmnopqrstuvwxyz";

        std::string result;
        result.reserve(length);
        
        for(int i = 0; i < length; i++) {
            result += s[random(0, sizeof(s) - 2)];
        }

        return result;
    }

    // 64位随机数生成（标识ID）
    static int64_t rand64() {
        return (static_cast<uint64_t>(random(0, INT_MAX)) << 32) | static_cast<uint64_t>(random(0, INT_MAX));
    }

    // 合法性验证
    static bool isIP(const std::string& ip) {
        std::vector<std::string> parts = split(ip, '.');
        if(parts.size() != 4) {
            return false;
        }
        for(const auto& p : parts) {
            if(p.empty() || p.size() > 3) {
                return false;
            }
            for(char c : p) {
                if(!isdigit(c)) {
                    return false;
                }
            }
            int num = std::stoi(p);
            if(num < 0 || num > 255) {
                return false;
            }
        }
        return true;
    }
};