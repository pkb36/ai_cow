#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <chrono>

template<typename T>
class SafeQueue {
public:
    void push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(value));
        }
        cv_.notify_one();
    }

    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front());
        queue_.pop();
        return value;
    }

    std::optional<T> popWait(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        if (timeout.count() > 0) {
            if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
                return std::nullopt;
            }
        } else {
            cv_.wait(lock, [this] { return !queue_.empty(); });
        }
        
        T value = std::move(queue_.front());
        queue_.pop();
        return value;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
};