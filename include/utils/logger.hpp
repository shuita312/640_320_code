#pragma once

#include <iostream>
#include <mutex>
#include <string>
#include <sstream>
#include "utils/time_sync.hpp"

namespace vio {
namespace utils {

// 定义日志级别
enum class LogLevel {
    DEBUG = 0,
    INFO,
    WARN,
    ERROR
};

/**
 * @brief 简单的终端日志工具类
 * 特性：线程安全、颜色区分、高精度时间戳
 */
class Logger {
public:
    // 获取单例实例 (Lazy Initialization)
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    // 禁用拷贝
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /**
     * @brief 核心日志输出函数
     */
    void log(LogLevel level, const std::string& file, int line, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_); // 保证多线程打印不乱序

        // 1. 打印时间戳 (使用系统单调时钟，单位：秒.毫秒)
        uint64_t now_ms = vio::time::get_sys_time_ms();
        double timestamp = now_ms / 1000.0;

        // 2. 根据级别设置颜色
        std::cout << "[" << timestamp << "] ";
        
        switch (level) {
            case LogLevel::DEBUG:
                std::cout << "\033[36m[DEBUG]\033[0m "; // 青色
                break;
            case LogLevel::INFO:
                std::cout << "\033[32m[INFO ]\033[0m "; // 绿色
                break;
            case LogLevel::WARN:
                std::cout << "\033[33m[WARN ]\033[0m "; // 黄色
                break;
            case LogLevel::ERROR:
                std::cout << "\033[31m[ERROR]\033[0m "; // 红色
                break;
        }

        // 3. 打印文件位置与消息内容
        std::cout << "[" << file << ":" << line << "] " << message << std::endl;
    }

private:
    Logger() = default;
    std::mutex mutex_; // 互斥锁，确保输出完整性
};

} // namespace utils
} // namespace vio

// ----------------------------------------------------------------------
// 便捷宏定义：自动填充文件名和行号
// ----------------------------------------------------------------------

#define VIO_LOG_DEBUG(msg) \
    { std::stringstream ss; ss << msg; \
      vio::utils::Logger::getInstance().log(vio::utils::LogLevel::DEBUG, __FILE__, __LINE__, ss.str()); }

#define VIO_LOG_INFO(msg) \
    { std::stringstream ss; ss << msg; \
      vio::utils::Logger::getInstance().log(vio::utils::LogLevel::INFO, __FILE__, __LINE__, ss.str()); }

#define VIO_LOG_WARN(msg) \
    { std::stringstream ss; ss << msg; \
      vio::utils::Logger::getInstance().log(vio::utils::LogLevel::WARN, __FILE__, __LINE__, ss.str()); }

#define VIO_LOG_ERROR(msg) \
    { std::stringstream ss; ss << msg; \
      vio::utils::Logger::getInstance().log(vio::utils::LogLevel::ERROR, __FILE__, __LINE__, ss.str()); }

// // 简单字符串
// VIO_LOG_INFO("System initialized successfully.");

// // 拼接变量
// int sensor_id = 5;
// double temperature = 42.5;
// VIO_LOG_WARN("Sensor " << sensor_id << " is getting hot: " << temperature << " C");

// // 报错
// VIO_LOG_ERROR("SD Card full! Cannot write packet.");