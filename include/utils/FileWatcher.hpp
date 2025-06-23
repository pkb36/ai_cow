#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <filesystem>
#include <chrono>
#include <unordered_map>

class FileWatcher {
public:
    using FileChangeCallback = std::function<void(const std::filesystem::path&, bool exists)>;

    FileWatcher();
    ~FileWatcher();

    // 파일/디렉토리 감시 추가
    void watch(const std::filesystem::path& path, FileChangeCallback callback);
    void unwatch(const std::filesystem::path& path);
    
    // 감시 시작/중지
    void start(std::chrono::milliseconds checkInterval = std::chrono::milliseconds(1000));
    void stop();

private:
    struct WatchInfo {
        std::filesystem::path path;
        FileChangeCallback callback;
        std::filesystem::file_time_type lastWriteTime;
        bool exists;
    };

    void watcherThread();
    void checkForChanges();

    std::atomic<bool> running_{false};
    std::thread watcherThread_;
    std::chrono::milliseconds checkInterval_{1000};
    
    std::unordered_map<std::string, WatchInfo> watchedPaths_;
    std::mutex pathsMutex_;
};