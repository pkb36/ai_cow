#include "core/Logger.hpp"
#include <iostream>
#include <iomanip>
#include <ctime>

Logger::~Logger() {
    if (fileStream_ && fileStream_->is_open()) {
        fileStream_->close();
    }
}

void Logger::setLogFile(const std::string& filename) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (fileStream_ && fileStream_->is_open()) {
        fileStream_->close();
    }
    
    fileStream_ = std::make_unique<std::ofstream>(filename, std::ios::app);
    if (!fileStream_->is_open()) {
        std::cerr << "Failed to open log file: " << filename << std::endl;
        fileStream_.reset();
    }
}

void Logger::writeLog(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string timestamp = getCurrentTimestamp();
    std::string levelStr = levelToString(level);
    std::string logLine = fmt::format("[{}] [{}] {}\n", timestamp, levelStr, message);
    
    // 콘솔 출력 (색상 포함)
    switch (level) {
        case LogLevel::TRACE:
            std::cout << "\033[90m" << logLine << "\033[0m";  // Dark gray
            break;
        case LogLevel::DEBUG:
            std::cout << "\033[36m" << logLine << "\033[0m";  // Cyan
            break;
        case LogLevel::INFO:
            std::cout << "\033[32m" << logLine << "\033[0m";  // Green
            break;
        case LogLevel::WARNING:
            std::cout << "\033[33m" << logLine << "\033[0m";  // Yellow
            break;
        case LogLevel::ERROR:
            std::cout << "\033[31m" << logLine << "\033[0m";  // Red
            break;
        case LogLevel::CRITICAL:
            std::cout << "\033[35m" << logLine << "\033[0m";  // Magenta
            break;
    }
    std::cout.flush();
    
    // 파일 출력
    if (fileStream_ && fileStream_->is_open()) {
        *fileStream_ << logLine;
        fileStream_->flush();
    }
}

std::string Logger::getCurrentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    
    return ss.str();
}

const char* Logger::levelToString(LogLevel level) const {
    switch (level) {
        case LogLevel::TRACE:    return "TRACE";
        case LogLevel::DEBUG:    return "DEBUG";
        case LogLevel::INFO:     return "INFO ";
        case LogLevel::WARNING:  return "WARN ";
        case LogLevel::ERROR:    return "ERROR";
        case LogLevel::CRITICAL: return "CRIT ";
        default:                 return "UNKN ";
    }
}