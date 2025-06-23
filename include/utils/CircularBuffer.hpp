#pragma once

#include <vector>
#include <mutex>
#include <algorithm>

template<typename T>
class CircularBuffer {
public:
    explicit CircularBuffer(size_t capacity) 
        : buffer_(capacity), capacity_(capacity), size_(0), head_(0), tail_(0) {}

    void push(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        buffer_[tail_] = std::move(item);
        tail_ = (tail_ + 1) % capacity_;
        
        if (size_ < capacity_) {
            size_++;
        } else {
            head_ = (head_ + 1) % capacity_;
        }
    }

    std::optional<T> pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (size_ == 0) {
            return std::nullopt;
        }
        
        T item = std::move(buffer_[head_]);
        head_ = (head_ + 1) % capacity_;
        size_--;
        
        return item;
    }

    std::vector<T> getAll() const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::vector<T> result;
        result.reserve(size_);
        
        size_t idx = head_;
        for (size_t i = 0; i < size_; ++i) {
            result.push_back(buffer_[idx]);
            idx = (idx + 1) % capacity_;
        }
        
        return result;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_ == 0;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        size_ = 0;
        head_ = 0;
        tail_ = 0;
    }

private:
    mutable std::mutex mutex_;
    std::vector<T> buffer_;
    size_t capacity_;
    size_t size_;
    size_t head_;
    size_t tail_;
};