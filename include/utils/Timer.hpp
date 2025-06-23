#pragma once

#include <chrono>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>

class Timer {
public:
    using Callback = std::function<void()>;
    
    Timer() = default;
    ~Timer() { stop(); }
    
    // 단발성 타이머
    void setTimeout(Callback callback, std::chrono::milliseconds delay) {
        stop();
        running_ = true;
        
        thread_ = std::make_unique<std::thread>([this, callback, delay]() {
            std::this_thread::sleep_for(delay);
            if (running_) {
                callback();
                running_ = false;
            }
        });
    }
    
    // 반복 타이머
    void setInterval(Callback callback, std::chrono::milliseconds interval) {
        stop();
        running_ = true;
        
        thread_ = std::make_unique<std::thread>([this, callback, interval]() {
            while (running_) {
                std::this_thread::sleep_for(interval);
                if (running_) {
                    callback();
                }
            }
        });
    }
    
    void stop() {
        running_ = false;
        if (thread_ && thread_->joinable()) {
            thread_->join();
        }
        thread_.reset();
    }
    
    bool isRunning() const { return running_; }
    
private:
    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> thread_;
};