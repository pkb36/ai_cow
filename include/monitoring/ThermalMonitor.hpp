#pragma once

#include <memory>
#include <functional>
#include <atomic>
#include <opencv2/opencv.hpp>

class ThermalMonitor {
public:
    struct ThermalConfig {
        int lowerThreshold = 15;    // °C
        int upperThreshold = 50;    // °C
        int tempDiffThreshold = 7;  // °C
        int overTempDuration = 15;  // seconds
        int tempCorrection = 0;     // °C
        bool enableTempDisplay = true;
        bool enableTempNotification = true;
    };

    struct ObjectTemperature {
        int objectId;
        float currentTemp;
        float averageTemp;
        float maxTemp;
        float minTemp;
        std::chrono::steady_clock::time_point lastUpdate;
        bool isOverTemp;
        int overTempDuration;
    };

    ThermalMonitor();
    ~ThermalMonitor();

    void setConfig(const ThermalConfig& config) { config_ = config; }
    
    // 프레임 처리
    void processFrame(const cv::Mat& thermalFrame, 
                     const std::vector<cv::Rect>& boundingBoxes,
                     const std::vector<int>& objectIds);
    
    // 온도 조회
    std::optional<ObjectTemperature> getObjectTemperature(int objectId) const;
    float getAverageSceneTemperature() const;
    std::vector<int> getOverTempObjects() const;

    // 콜백
    using OverTempCallback = std::function<void(int objectId, float temperature)>;
    void setOverTempCallback(OverTempCallback cb) { overTempCallback_ = cb; }

private:
    float pixelToTemperature(const cv::Vec3b& pixel) const;
    void updateObjectTemperature(int objectId, float temp);
    void checkOverTempConditions();

    ThermalConfig config_;
    std::unordered_map<int, ObjectTemperature> objectTemps_;
    mutable std::mutex tempMutex_;
    
    OverTempCallback overTempCallback_;
    
    // 보정 테이블
    std::vector<std::pair<cv::Rect, float>> correctionZones_;
};