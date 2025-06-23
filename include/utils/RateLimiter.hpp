#pragma once

#include <chrono>
#include <mutex>
#include <queue>

class RateLimiter {
public:
    RateLimiter(size_t maxRequests, std::chrono::seconds window)
        : maxRequests_(maxRequests), window_(window) {}
    
    bool allowRequest() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto now = std::chrono::steady_clock::now();
        
        // 오래된 요청 제거
        while (!requests_.empty()) {
            auto age = now - requests_.front();
            if (age > window_) {
                requests_.pop();
            } else {
                break;
            }
        }
        
        // 요청 허용 여부 확인
        if (requests_.size() < maxRequests_) {
            requests_.push(now);
            return true;
        }
        
        return false;
    }
    
    size_t getCurrentRequests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.size();
    }
    
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<std::chrono::steady_clock::time_point> empty;
        requests_.swap(empty);
    }
    
private:
    size_t maxRequests_;
    std::chrono::seconds window_;
    mutable std::mutex mutex_;
    std::queue<std::chrono::steady_clock::time_point> requests_;
};