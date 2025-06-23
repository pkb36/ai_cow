#pragma once

#include <memory>
#include <thread>
#include <atomic>
#include <functional>

class SystemMonitor {
public:
    struct SystemStatus {
        // CPU/GPU
        int cpuTemp;
        int gpuTemp;
        float cpuUsage;
        float gpuUsage;
        
        // 메모리
        size_t totalMemory;
        size_t usedMemory;
        size_t availableMemory;
        
        // 스토리지
        size_t totalStorage;
        size_t usedStorage;
        int storageUsagePercent;
        
        // 네트워크
        uint64_t networkTxBytes;
        uint64_t networkRxBytes;
        float networkTxRate;  // Mbps
        float networkRxRate;  // Mbps
        
        // 프로세스
        size_t processMemory;  // KB
        int processCpuPercent;
        int threadCount;
    };

    struct AlertThresholds {
        int maxCpuTemp = 85;
        int maxGpuTemp = 85;
        int maxMemoryPercent = 90;
        int maxStoragePercent = 95;
        size_t minAvailableStorage = 1024 * 1024 * 1024; // 1GB
    };

    static SystemMonitor& getInstance() {
        static SystemMonitor instance;
        return instance;
    }

    void start(std::chrono::seconds interval = std::chrono::seconds(5));
    void stop();
    
    SystemStatus getCurrentStatus() const;
    void setAlertThresholds(const AlertThresholds& thresholds);
    
    // 알림 콜백
    using AlertCallback = std::function<void(const std::string& alert)>;
    void setAlertCallback(AlertCallback cb) { alertCallback_ = cb; }

private:
    SystemMonitor() = default;
    ~SystemMonitor();

    void monitoringThread();
    void updateStatus();
    void checkAlerts();
    
    // 시스템 정보 읽기
    int readTemperature(const std::string& zone);
    float readCpuUsage();
    void readMemoryInfo();
    void readStorageInfo();
    void readNetworkStats();
    void readProcessInfo();

    std::atomic<bool> running_{false};
    std::thread monitorThread_;
    std::chrono::seconds interval_{5};
    
    mutable std::mutex statusMutex_;
    SystemStatus currentStatus_;
    SystemStatus previousStatus_;
    
    AlertThresholds thresholds_;
    AlertCallback alertCallback_;
    
    // 네트워크 통계용
    std::chrono::steady_clock::time_point lastNetworkCheck_;
};