#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <memory>
#include <sstream>
#include <chrono>
#include <fmt/format.h>

// DEBUG 매크로 충돌 방지
#ifdef DEBUG
#undef DEBUG
#endif

enum class LogLevel {
    TRACE = 0,
    DEBUG_LEVEL,  // DEBUG 대신 DEBUG_LEVEL 사용
    INFO,
    WARNING,
    ERROR,
    CRITICAL
};

class Logger {
public:
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    void setLogLevel(LogLevel level) { minLevel_ = level; }
    void setLogFile(const std::string& filename);
    
    template<typename... Args>
    void log(LogLevel level, const std::string& format, Args&&... args) {
        if (level < minLevel_) return;
        
        std::string message;
        if constexpr (sizeof...(args) == 0) {
            message = format;
        } else {
            message = fmt::format(format, std::forward<Args>(args)...);
        }
        writeLog(level, message);
    }

private:
    Logger() = default;
    ~Logger();

    void writeLog(LogLevel level, const std::string& message);
    std::string getCurrentTimestamp() const;
    const char* levelToString(LogLevel level) const;

    std::mutex mutex_;
    std::unique_ptr<std::ofstream> fileStream_;
    LogLevel minLevel_ = LogLevel::TRACE;
};

// 전역 로거 매크로
#define LOG_TRACE(...) Logger::getInstance().log(LogLevel::TRACE, __VA_ARGS__)
#define LOG_DEBUG(...) Logger::getInstance().log(LogLevel::DEBUG_LEVEL, __VA_ARGS__)
#define LOG_INFO(...) Logger::getInstance().log(LogLevel::INFO, __VA_ARGS__)
#define LOG_WARNING(...) Logger::getInstance().log(LogLevel::WARNING, __VA_ARGS__)
#define LOG_ERROR(...) Logger::getInstance().log(LogLevel::ERROR, __VA_ARGS__)
#define LOG_CRITICAL(...) Logger::getInstance().log(LogLevel::CRITICAL, __VA_ARGS__)

// LOG_LEVEL_DEBUG 정의 (SerialPort.cpp에서 사용)
#ifdef NDEBUG
#define LOG_LEVEL_DEBUG 0
#else
#define LOG_LEVEL_DEBUG 1
#endif