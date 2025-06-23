#include "utils/Performance.hpp"
#include "core/Logger.hpp"
#include <algorithm>

void PerformanceMonitor::recordMetric(const std::string& name, int64_t microseconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto& metric = metrics_[name];
    metric.count++;
    metric.totalTime += microseconds;
    metric.minTime = std::min(metric.minTime, microseconds);
    metric.maxTime = std::max(metric.maxTime, microseconds);
    metric.avgTime = static_cast<double>(metric.totalTime) / metric.count;
}

PerformanceMonitor::Metrics PerformanceMonitor::getMetrics(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = metrics_.find(name);
    if (it != metrics_.end()) {
        return it->second;
    }
    
    return Metrics{};
}

std::unordered_map<std::string, PerformanceMonitor::Metrics> 
PerformanceMonitor::getAllMetrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}

void PerformanceMonitor::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.clear();
    LOG_INFO("Performance metrics reset");
}