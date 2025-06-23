#include "utils/ThreadPool.hpp"
#include "core/Logger.hpp"

ThreadPool::ThreadPool(size_t numThreads) {
    LOG_INFO("Creating thread pool with {} threads", numThreads);
    
    workers_.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        workers_.emplace_back(&ThreadPool::workerThread, this);
    }
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        stop_ = true;
    }
    
    condition_.notify_all();
    
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    
    LOG_INFO("Thread pool destroyed");
}

void ThreadPool::wait() {
    std::unique_lock<std::mutex> lock(queueMutex_);
    finished_.wait(lock, [this] { 
        return tasks_.empty() && activeTasks_ == 0; 
    });
}

size_t ThreadPool::getTaskCount() const {
    std::unique_lock<std::mutex> lock(queueMutex_);
    return tasks_.size() + activeTasks_;
}

void ThreadPool::workerThread() {
    while (true) {
        std::function<void()> task;
        
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            condition_.wait(lock, [this] { 
                return stop_ || !tasks_.empty(); 
            });
            
            if (stop_ && tasks_.empty()) {
                return;
            }
            
            task = std::move(tasks_.front());
            tasks_.pop();
            activeTasks_++;
        }
        
        try {
            task();
        } catch (const std::exception& e) {
            LOG_ERROR("Exception in thread pool task: {}", e.what());
        }
        
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            activeTasks_--;
            if (tasks_.empty() && activeTasks_ == 0) {
                finished_.notify_all();
            }
        }
    }
}