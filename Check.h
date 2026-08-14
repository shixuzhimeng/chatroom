#pragma once

#include <regex>
#include <string>
#include <vector>
#include <cctype>
#include "logging.h"

class InputValidator {
public:
    // 用户名验证
    static bool validateUsername(const std::string& username) {
        if (username.length() < 3 || username.length() > 20) {
            LOG_ERROR << "Username length invalid: " << username.length();
            return false;
        }
        std::regex pattern("^[a-zA-Z0-9_]+$");
        if (!std::regex_match(username, pattern)) {
            LOG_ERROR << "Username contains invalid characters";
            return false;
        }
        return true;
    }

    // 密码验证
    static bool validatePassword(const std::string& password) {
        if (password.length() < 8 || password.length() > 32) {
            LOG_ERROR << "Password length invalid: " << password.length();
            return false;
        }
        bool has_letter = false, has_digit = false;
        for (char c : password) {
            if (std::isalpha(c)) has_letter = true;
            if (std::isdigit(c)) has_digit = true;
            if (!std::isprint(c)) {
                LOG_ERROR << "Password contains non-printable char";
                return false;
            }
        }
        if (!has_letter || !has_digit) {
            LOG_ERROR << "Password must contain both letters and digits";
            return false;
        }
        return true;
    }

    // 邮箱验证
    static bool validateEmail(const std::string& email) {
        if (email.empty()) return true; // 允许为空
        if (email.length() > 100) {
            LOG_ERROR << "Email too long";
            return false;
        }
        // 简单但有效的邮箱正则
        std::regex pattern(R"((\w+)(\.\w+)*@(\w+)(\.\w+)+)");
        if (!std::regex_match(email, pattern)) {
            LOG_ERROR << "Invalid email format: " << email;
            return false;
        }
        return true;
    }

    // 手机号验证（中国）
    static bool validatePhone(const std::string& phone) {
        if (phone.empty()) return true;
        if (phone.length() != 11) {
            LOG_ERROR << "Phone length invalid: " << phone.length();
            return false;
        }
        std::regex pattern("^1[3-9]\\d{9}$");
        if (!std::regex_match(phone, pattern)) {
            LOG_ERROR << "Invalid phone format: " << phone;
            return false;
        }
        return true;
    }

    // 昵称验证
    static bool validateNickname(const std::string& nickname) {
        if (nickname.empty()) {
            LOG_ERROR << "Nickname cannot be empty";
            return false;
        }
        if (nickname.length() > 50) {
            LOG_ERROR << "Nickname too long: " << nickname.length();
            return false;
        }
        // 不允许控制字符
        for (char c : nickname) {
            if (static_cast<unsigned char>(c) < 0x20 && c != ' ') {
                LOG_ERROR << "Nickname contains control char";
                return false;
            }
        }
        return true;
    }

    // 群组名称验证
    static bool validateGroupName(const std::string& name) {
        if (name.empty() || name.length() > 100) {
            LOG_ERROR << "Group name length invalid: " << name.length();
            return false;
        }
        for (char c : name) {
            if (static_cast<unsigned char>(c) < 0x20 && c != ' ') {
                LOG_ERROR << "Group name contains control char";
                return false;
            }
        }
        return true;
    }

    // 消息内容验证
    static bool validateMessageContent(const std::string& content, size_t max_len = 4096) {
        if (content.empty()) {
            LOG_ERROR << "Message content empty";
            return false;
        }
        if (content.length() > max_len) {
            LOG_ERROR << "Message too long: " << content.length() << " > " << max_len;
            return false;
        }
        // 检查是否包含非法字符（如控制字符，但允许换行）
        for (char c : content) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (uc < 0x20 && c != '\n' && c != '\r' && c != '\t') {
                LOG_ERROR << "Message contains control char: " << (int)uc;
                return false;
            }
        }
        return true;
    }

    // 文件相关验证
    static bool validateFilename(const std::string& filename) {
        if (filename.empty() || filename.length() > 255) {
            LOG_ERROR << "Filename length invalid";
            return false;
        }
        // 禁止路径遍历字符
        if (filename.find('/') != std::string::npos || 
            filename.find('\\') != std::string::npos ||
            filename.find("..") != std::string::npos) {
            LOG_ERROR << "Filename contains path traversal: " << filename;
            return false;
        }
        return true;
    }

    static bool validateFileSize(uint64_t size, uint64_t max_size = 100 * 1024 * 1024) {
        if (size == 0) {
            LOG_ERROR << "File size is zero";
            return false;
        }
        if (size > max_size) {
            LOG_ERROR << "File too large: " << size << " > " << max_size;
            return false;
        }
        return true;
    }

    // 通用长度验证
    static bool validateLength(const std::string& str, size_t min, size_t max) {
        if (str.length() < min || str.length() > max) {
            LOG_ERROR << "String length " << str.length() << " out of range [" << min << "," << max << "]";
            return false;
        }
        return true;
    }

    // 数字范围验证
    template<typename T>
    static bool validateRange(T value, T min, T max) {
        if (value < min || value > max) {
            LOG_ERROR << "Value " << value << " out of range [" << min << "," << max << "]";
            return false;
        }
        return true;
    }

    // 防SQL注入
    static bool hasSQLInjectionRisk(const std::string& input) {
        // 检查危险关键字（不区分大小写）
        static const std::vector<std::string> dangerous = {
            "SELECT", "INSERT", "UPDATE", "DELETE", "DROP", "ALTER",
            "CREATE", "TRUNCATE", "UNION", "EXEC", "EXECUTE",
            "--", "/*", "*/", ";"
        };
        std::string upper = input;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        for (const auto& keyword : dangerous) {
            if (upper.find(keyword) != std::string::npos) {
                LOG_ERROR << "Potential SQL injection detected: " << keyword;
                return true;
            }
        }
        return false;
    }

    // 综合输入校验（用于登录/注册等）
    struct ValidationResult {
        bool valid = true;
        std::string error;
    };

    static ValidationResult validateLoginInput(const std::string& username, const std::string& password) {
        ValidationResult result;
        if (!validateUsername(username)) {
            result.valid = false;
            result.error = "Invalid username format";
            return result;
        }
        if (!validatePassword(password)) {
            result.valid = false;
            result.error = "Invalid password format";
            return result;
        }
        return result;
    }

    static ValidationResult validateRegisterInput(const std::string& username, 
                                                  const std::string& password,
                                                  const std::string& email,
                                                  const std::string& nickname) {
        ValidationResult result;
        if (!validateUsername(username)) {
            result.valid = false;
            result.error = "Invalid username format (3-20 chars, letters/digits/_)";
            return result;
        }
        if (!validatePassword(password)) {
            result.valid = false;
            result.error = "Invalid password format (8-32 chars, letters and digits)";
            return result;
        }
        if (!validateEmail(email)) {
            result.valid = false;
            result.error = "Invalid email format";
            return result;
        }
        if (!validateNickname(nickname)) {
            result.valid = false;
            result.error = "Invalid nickname format";
            return result;
        }
        return result;
    }
};