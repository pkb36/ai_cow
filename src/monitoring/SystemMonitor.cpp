#include "monitoring/SystemMonitor.hpp"
#include "core/Logger.hpp"
#include <fstream>
#include <sstream>

SystemMonitor::~SystemMonitor() {
    stop();
}

void SystemMonitor::start(std::chrono::seconds interval) {
    if (running_) return;
    
    interval_ = interval;
    running_ = true;
    monitorThread_ = std::thread(&SystemMonitor::monitoringThread, this);
}

void SystemMonitor::stop() {
    running_ = false;
    if (monitorThread_.joinable()) {
        monitorThread_.join();
    }
}

SystemMonitor::SystemStatus SystemMonitor::getCurrentStatus() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return currentStatus_;
}

void SystemMonitor::monitoringThread() {
    while (running_) {
        updateStatus();
        checkAlerts();
        std::this_thread::sleep_for(interval_);
    }
}

void SystemMonitor::updateStatus() {
    std::lock_guard<std::mutex> lock(statusMutex_);
    
    // CPU 온도 읽기
    currentStatus_.cpuTemp = readTemperature("/sys/class/thermal/thermal_zone0/temp");
    currentStatus_.gpuTemp = readTemperature("/sys/class/thermal/thermal_zone1/temp");
    
    // 메모리 정보
    readMemoryInfo();
    
    // CPU 사용률
    currentStatus_.cpuUsage = readCpuUsage();
}

int SystemMonitor::readTemperature(const std::string& path) {
    std::ifstream file(path);
    if (!file) return 0;
    
    int temp;
    file >> temp;
    return temp / 1000; // millidegree to degree
}

void SystemMonitor::readMemoryInfo() {
    std::ifstream file("/proc/meminfo");
    if (!file) return;
    
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string key;
        long value;
        std::string unit;
        
        iss >> key >> value >> unit;
        
        if (key == "MemTotal:") {
            currentStatus_.totalMemory = value * 1024;
        } else if (key == "MemAvailable:") {
            currentStatus_.availableMemory = value * 1024;
        }
    }
    
    currentStatus_.usedMemory = currentStatus_.totalMemory - currentStatus_.availableMemory;
}

float SystemMonitor::readCpuUsage() {
    static long lastTotalTime = 0;
    static long lastIdleTime = 0;
    
    std::ifstream file("/proc/stat");
    if (!file) return 0.0f;
    
    std::string cpu;
    long user, nice, system, idle, iowait, irq, softirq, steal;
    
    file >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
    
    long totalTime = user + nice + system + idle + iowait + irq + softirq + steal;
    long deltaTotal = totalTime - lastTotalTime;
    long deltaIdle = idle - lastIdleTime;
    
    lastTotalTime = totalTime;
    lastIdleTime = idle;
    
    if (deltaTotal == 0) return 0.0f;
    
    return 100.0f * (1.0f - static_cast<float>(deltaIdle) / deltaTotal);
}

void SystemMonitor::checkAlerts() {
    const auto& status = currentStatus_;
    
    if (status.cpuTemp > thresholds_.maxCpuTemp) {
        if (alertCallback_) {
            alertCallback_("CPU temperature critical: " + std::to_string(status.cpuTemp) + "°C");
        }
    }
    
    if (status.gpuTemp > thresholds_.maxGpuTemp) {
        if (alertCallback_) {
            alertCallback_("GPU temperature critical: " + std::to_string(status.gpuTemp) + "°C");
        }
    }
}