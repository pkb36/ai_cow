#include "utils/FileWatcher.hpp"
#include "core/Logger.hpp"
#include <sys/inotify.h>
#include <unistd.h>
#include <cstring>

FileWatcher::FileWatcher() = default;

FileWatcher::~FileWatcher() {
    stop();
}

void FileWatcher::watch(const std::filesystem::path& path, FileChangeCallback callback) {
    std::lock_guard<std::mutex> lock(pathsMutex_);
    
    WatchInfo info;
    info.path = path;
    info.callback = callback;
    info.exists = std::filesystem::exists(path);
    
    if (info.exists) {
        try {
            info.lastWriteTime = std::filesystem::last_write_time(path);
        } catch (const std::exception& e) {
            LOG_WARNING("Failed to get last write time for {}: {}", path.string(), e.what());
        }
    }
    
    watchedPaths_[path.string()] = info;
    LOG_INFO("Watching path: {} (exists: {})", path.string(), info.exists);
}

void FileWatcher::unwatch(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(pathsMutex_);
    watchedPaths_.erase(path.string());
    LOG_INFO("Stopped watching path: {}", path.string());
}

void FileWatcher::start(std::chrono::milliseconds checkInterval) {
    if (running_) {
        LOG_WARNING("FileWatcher already running");
        return;
    }
    
    checkInterval_ = checkInterval;
    running_ = true;
    watcherThread_ = std::thread(&FileWatcher::watcherThread, this);
    LOG_INFO("FileWatcher started with interval: {}ms", checkInterval.count());
}

void FileWatcher::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    if (watcherThread_.joinable()) {
        watcherThread_.join();
    }
    LOG_INFO("FileWatcher stopped");
}

void FileWatcher::watcherThread() {
    while (running_) {
        checkForChanges();
        std::this_thread::sleep_for(checkInterval_);
    }
}

void FileWatcher::checkForChanges() {
    std::lock_guard<std::mutex> lock(pathsMutex_);
    
    for (auto& [pathStr, info] : watchedPaths_) {
        try {
            bool currentlyExists = std::filesystem::exists(info.path);
            
            // 파일 존재 상태 변경 확인
            if (currentlyExists != info.exists) {
                info.exists = currentlyExists;
                if (info.callback) {
                    info.callback(info.path, currentlyExists);
                }
                LOG_DEBUG("File {} existence changed to: {}", pathStr, currentlyExists);
                continue;
            }
            
            // 파일이 존재하면 수정 시간 확인
            if (currentlyExists) {
                auto currentWriteTime = std::filesystem::last_write_time(info.path);
                if (currentWriteTime != info.lastWriteTime) {
                    info.lastWriteTime = currentWriteTime;
                    if (info.callback) {
                        info.callback(info.path, true);
                    }
                    LOG_DEBUG("File {} was modified", pathStr);
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Error checking file {}: {}", pathStr, e.what());
        }
    }
}