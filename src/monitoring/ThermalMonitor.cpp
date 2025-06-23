#include "monitoring/ThermalMonitor.hpp"
#include "core/Logger.hpp"

ThermalMonitor::ThermalMonitor() {
    LOG_TRACE("ThermalMonitor created");
}

ThermalMonitor::~ThermalMonitor() = default;

void ThermalMonitor::processFrame(const cv::Mat& thermalFrame, 
                                 const std::vector<cv::Rect>& boundingBoxes,
                                 const std::vector<int>& objectIds) {
    if (thermalFrame.empty()) {
        return;
    }
    
    // 각 객체에 대한 온도 측정
    for (size_t i = 0; i < boundingBoxes.size() && i < objectIds.size(); ++i) {
        const cv::Rect& bbox = boundingBoxes[i];
        int objectId = objectIds[i];
        
        // 바운딩 박스가 프레임 범위 내에 있는지 확인
        cv::Rect validBbox = bbox & cv::Rect(0, 0, thermalFrame.cols, thermalFrame.rows);
        if (validBbox.empty()) {
            continue;
        }
        
        // ROI 추출
        cv::Mat roi = thermalFrame(validBbox);
        
        // 온도 계산
        float minTemp = std::numeric_limits<float>::max();
        float maxTemp = std::numeric_limits<float>::min();
        float sumTemp = 0.0f;
        int pixelCount = 0;
        
        for (int y = 0; y < roi.rows; ++y) {
            for (int x = 0; x < roi.cols; ++x) {
                cv::Vec3b pixel = roi.at<cv::Vec3b>(y, x);
                float temp = pixelToTemperature(pixel);
                
                minTemp = std::min(minTemp, temp);
                maxTemp = std::max(maxTemp, temp);
                sumTemp += temp;
                pixelCount++;
            }
        }
        
        if (pixelCount > 0) {
            float avgTemp = sumTemp / pixelCount;
            
            // 보정 적용
            avgTemp += config_.tempCorrection;
            minTemp += config_.tempCorrection;
            maxTemp += config_.tempCorrection;
            
            // 객체 온도 업데이트
            updateObjectTemperature(objectId, avgTemp);
        }
    }
    
    // 과온 상태 확인
    checkOverTempConditions();
}

float ThermalMonitor::pixelToTemperature(const cv::Vec3b& pixel) const {
    // 열화상 카메라의 픽셀값을 온도로 변환
    // 이는 카메라 모델과 설정에 따라 다름
    
    // 예시: 단순 선형 변환 (실제로는 카메라 캘리브레이션 필요)
    // 픽셀값 0-255를 온도 범위로 매핑
    float normalized = pixel[0] / 255.0f;  // R 채널 사용
    float temp = config_.lowerThreshold + 
                 (config_.upperThreshold - config_.lowerThreshold) * normalized;
    
    return temp;
}

void ThermalMonitor::updateObjectTemperature(int objectId, float temp) {
    std::lock_guard<std::mutex> lock(tempMutex_);
    
    auto& objTemp = objectTemps_[objectId];
    objTemp.objectId = objectId;
    objTemp.currentTemp = temp;
    objTemp.lastUpdate = std::chrono::steady_clock::now();
    
    // 최대/최소 온도 업데이트
    if (objTemp.maxTemp == 0 || temp > objTemp.maxTemp) {
        objTemp.maxTemp = temp;
    }
    if (objTemp.minTemp == 0 || temp < objTemp.minTemp) {
        objTemp.minTemp = temp;
    }
    
    // 평균 온도 계산 (이동 평균)
    if (objTemp.averageTemp == 0) {
        objTemp.averageTemp = temp;
    } else {
        objTemp.averageTemp = objTemp.averageTemp * 0.9f + temp * 0.1f;
    }
    
    // 과온 상태 확인
    bool wasOverTemp = objTemp.isOverTemp;
    objTemp.isOverTemp = (temp > config_.upperThreshold);
    
    if (objTemp.isOverTemp && !wasOverTemp) {
        // 과온 시작
        objTemp.overTempDuration = 0;
        LOG_WARNING("Object {} temperature exceeded threshold: {:.1f}°C", 
                   objectId, temp);
    } else if (!objTemp.isOverTemp && wasOverTemp) {
        // 과온 종료
        objTemp.overTempDuration = 0;
        LOG_INFO("Object {} temperature returned to normal: {:.1f}°C", 
                objectId, temp);
    }
}

void ThermalMonitor::checkOverTempConditions() {
    std::lock_guard<std::mutex> lock(tempMutex_);
    
    auto now = std::chrono::steady_clock::now();
    
    // 오래된 객체 제거 (30초 이상 업데이트 없음)
    for (auto it = objectTemps_.begin(); it != objectTemps_.end();) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.lastUpdate).count();
        
        if (elapsed > 30) {
            LOG_DEBUG("Removing stale object temperature: {}", it->first);
            it = objectTemps_.erase(it);
        } else {
            ++it;
        }
    }
    
    // 과온 지속 시간 확인
    for (auto& [objectId, objTemp] : objectTemps_) {
        if (objTemp.isOverTemp) {
            objTemp.overTempDuration++;
            
            // 설정된 시간 이상 과온 상태가 지속되면 알림
            if (objTemp.overTempDuration == config_.overTempDuration) {
                LOG_ERROR("Object {} has been over temperature for {} seconds at {:.1f}°C", 
                         objectId, config_.overTempDuration, objTemp.currentTemp);
                
                if (overTempCallback_) {
                    overTempCallback_(objectId, objTemp.currentTemp);
                }
            }
        }
    }
}

std::optional<ThermalMonitor::ObjectTemperature> 
ThermalMonitor::getObjectTemperature(int objectId) const {
    std::lock_guard<std::mutex> lock(tempMutex_);
    
    auto it = objectTemps_.find(objectId);
    if (it != objectTemps_.end()) {
        return it->second;
    }
    
    return std::nullopt;
}

float ThermalMonitor::getAverageSceneTemperature() const {
    std::lock_guard<std::mutex> lock(tempMutex_);
    
    if (objectTemps_.empty()) {
        return 0.0f;
    }
    
    float sum = 0.0f;
    for (const auto& [id, temp] : objectTemps_) {
        sum += temp.currentTemp;
    }
    
    return sum / objectTemps_.size();
}

std::vector<int> ThermalMonitor::getOverTempObjects() const {
    std::lock_guard<std::mutex> lock(tempMutex_);
    
    std::vector<int> overTempIds;
    
    for (const auto& [id, temp] : objectTemps_) {
        if (temp.isOverTemp) {
            overTempIds.push_back(id);
        }
    }
    
    return overTempIds;
}