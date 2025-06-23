#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>

class PerformanceMonitor {
public:
    static PerformanceMonitor& getInstance() {
        static PerformanceMonitor instance;
        return instance;
    }

    class ScopedTimer {
    public:
        ScopedTimer(const std::string& name) 
            : name_(name), start_(std::chrono::high_resolution_clock::now()) {}
        
        ~ScopedTimer() {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start_);
            PerformanceMonitor::getInstance().recordMetric(name_, duration.count());
        }

    private:
        std::string name_;
        std::chrono::high_resolution_clock::time_point start_;
    };

    void recordMetric(const std::string& name, int64_t microseconds);
    
    struct Metrics {
        int64_t count = 0;
        int64_t totalTime = 0;
        int64_t minTime = std::numeric_limits<int64_t>::max();
        int64_t maxTime = 0;
        double avgTime = 0.0;
    };
    
    Metrics getMetrics(const std::string& name) const;
    std::unordered_map<std::string, Metrics> getAllMetrics() const;
    void reset();

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Metrics> metrics_;
};

// 매크로로 쉽게 사용
#define PERF_TIMER(name) PerformanceMonitor::ScopedTimer _timer(name)