#pragma once

#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>

using json = nlohmann::json;

class Config{
public:
    Config() = default;

    static Config& getInstance() {
        static Config instance;
        return instance;
    }

    // 加载配置文件.json
    bool load(const std::string& filepath) {
        try {
            std::ifstream file(filepath);
            if(!file.is_open()) {
                std::cerr << "Failed to open file: " << filepath << std::endl;
                return false;
            }
            file >> config_;
            return true;
        }
        catch(const std::exception& e) {
            std::cerr << "Config load failed " << e.what() << std::endl;
            return false;
        }
    }

    template<typename T>
    T get(const std::string& key, const T& default_value = T()) const {
        try {
            const json* current = &config_;
            size_t start = 0, end = key.find('.');
            while (end != std::string::npos) {
                std::string part = key.substr(start, end - start);
                if (!current->contains(part)) {
                    return default_value;
                }
                current = &(*current)[part];
                start = end + 1;
                end = key.find('.', start);
            }
            std::string last_key = key.substr(start);
            if (current->contains(last_key)) {
                return (*current)[last_key].get<T>();
            }
            return default_value;
        } catch (...) {
            return default_value;
        }
    }

    template<typename T>
    void set(const std::string& key, const T& value) {
        json* current = &config_;
        size_t start = 0;
        size_t end = key.find('.');

        while (end != std::string::npos) {
            std::string part = key.substr(start, end - start);
            if (!current->contains(part)) {
                (*current)[part] = json::object();
            }
            current = &(*current)[part];
            start = end + 1;
            end = key.find('.', start);
        }

        std::string last_key = key.substr(start);
        (*current)[last_key] = value;
    }

    std::string getString(const std::string& key, const std::string& default_value = "") const{
        return get<std::string>(key, default_value);
    }

    int getInt(const std::string& key, int default_value = 0) const {
        return get<int>(key, default_value);
    }

    bool getBool(const std::string& key, bool default_value = false) const{
        return get<bool>(key, default_value);
    }

    json getJson(const std::string& key) const{
        if(config_.contains(key)) {
            return config_[key];
        }
        return json::object();
    }


private:
    json config_;
};

#define LOAD_CONFIG(path) Config::getInstance().load(path)
#define GET_CONFIG(key, default_val) Config::getInstance().get(key, default_val)