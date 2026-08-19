#pragma once

#include <glog/logging.h>
#include <string>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

#define LOG_INFO LOG(INFO) << "[INFO] "
#define LOG_WARN LOG(WARNING) << "[WARN] "
#define LOG_ERROR LOG(ERROR) << "[ERROR] "
#define LOG_FATAL LOG(FATAL) << "[FATAL] "

#define LOG_DEBUG DLOG(INFO) << "[DEBUG] "


// 条件日志
#define LOG_IF_DEBUG(condition) LOG_IF(INFO, condition) << "[DEBUG] "
#define LOG_IF_INFO(condition) LOG_IF(INFO, condition) << "[INFO] "
#define LOG_IF_WARN(condition) LOG_IF(WARNING, condition) << "[WARN] "
#define LOG_IF_ERROR(condition) LOG_IF(ERROR, condition) << "[ERROR] "

// 每N次日志
#define LOG_EVERY_N_DEBUG(n) LOG_EVERY_N(INFO, n) << "[DEBUG] "
#define LOG_EVERY_N_INFO(n) LOG_EVERY_N(INFO, n) << "[INFO] "
#define LOG_EVERY_N_WARN(n) LOG_EVERY_N(WARNING, n) << "[WARN] "
#define LOG_EVERY_N_ERROR(n) LOG_EVERY_N(ERROR, n) << "[ERROR] "

inline void LOGinit(const std::string& program_name, const std::string& log_dir = "./logs", bool logtostderr = false) {
    google::InitGoogleLogging(program_name.c_str());

    // 设置日志的目录
    FLAGS_log_dir = log_dir;
    FLAGS_logtostderr = logtostderr;

    // 设置最低级别的日志输出
    FLAGS_minloglevel = google::INFO;

    // 设置日志文件的大小
    FLAGS_max_log_size = 100;

    // 保留日志文件的数量
    FLAGS_logbufsecs = 0;

    // 日志颜色区分
    FLAGS_colorlogtostderr = logtostderr;


    LOG_INFO << "LOG system init , Program: " << program_name;
}
