#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <nlohmann/json.hpp>

class Config {
public:
    // 비디오 설정 구조체
    struct VideoConfig {
        std::string src;
        std::string record;
        std::string infer;
        std::string enc;
        std::string enc2;
        std::string snapshot;
    };

    // WebRTC 설정 구조체
    struct WebRTCConfig {
        // 기본 설정
        std::string cameraId;
        std::string serverIp;
        int commSocketPort = 6000;
        int maxStreamCount = 10;
        int streamBasePort = 5000;
        int deviceCnt = 2;
        
        // 경로 설정
        std::string snapshotPath = "/home/nvidia/webrtc";
        std::string recordPath = "/home/nvidia/data";
        std::string deviceSettingPath = "/home/nvidia/webrtc/device_setting.json";
        
        // 녹화 설정
        int recordDuration = 5;  // 분 단위
        int recordEncIndex = 1;
        int eventRecordEncIndex = 0;
        int eventBufTime = 15;
        
        // 서버 설정
        std::string eventUserId = "itechour";
        std::string eventUserPw = "12341234";
        std::string eventServerIp = "52.194.238.184";
        
        // 기타 설정
        int statusTimerInterval = 1000;
        std::string httpServicePort = "9617";
        
        // TTY 설정
        std::string ttyName = "/dev/ttyTHS0";
        int ttyBaudrate = 38400;
        
        // 비디오 설정들 (video0, video1)
        VideoConfig video[2];
    };

    // 디바이스 설정 구조체
    struct DeviceSettings {
        // 기본 설정
        int colorPalette = 6;
        bool recordStatus = true;
        bool analysisStatus = true;
        
        // PTZ 설정
        std::string autoPtzSeq = "0,1,2,3,4,5,6,7,8,9,FF,15";
        std::vector<std::string> ptzPreset;
        std::vector<std::string> autoPtzPreset;
        int autoPtzMoveSpeed = 48;
        int ptzMoveSpeed = 48;
        
        // 이벤트 설정
        bool enableEventNotify = true;
        int cameraDnMode = 1;
        int nvInterval = 2;
        
        // 옵티컬 플로우 설정
        int optFlowThreshold = 11;
        bool optFlowApply = true;
        
        // ResNet50 설정
        int resnet50Threshold = 6;
        bool resnet50Apply = false;
        
        // 임계값 설정
        int normalThreshold = 30;
        int heatThreshold = 101;
        int flipThreshold = 80;
        int laborSignThreshold = 101;
        int normalSittingThreshold = 25;
        
        // 시간 설정
        int heatTime = 15;
        int flipTime = 15;
        int laborSignTime = 15;
        int overTempTime = 15;
        
        // 온도 설정
        bool tempApply = true;
        bool displayTemp = true;
        int tempDiffThreshold = 12;
        int tempCorrection = 8;
        int thresholdUpperTemp = 35;
        int thresholdUnderTemp = 15;
        
        // 기타 설정
        int cameraIndex = 0;
        bool showNormalText = false;
    };

    static Config& getInstance() {
        static Config instance;
        return instance;
    }

    bool loadConfig(const std::filesystem::path& configPath);
    bool loadDeviceSettings(const std::filesystem::path& settingsPath);
    bool saveDeviceSettings();

    const WebRTCConfig& getWebRTCConfig() const { return webrtcConfig_; }
    const DeviceSettings& getDeviceSettings() const { return deviceSettings_; }
    DeviceSettings& getDeviceSettings() { return deviceSettings_; }

private:
    Config() = default;
    
    WebRTCConfig webrtcConfig_;
    DeviceSettings deviceSettings_;
    std::filesystem::path deviceSettingsPath_;

    // JSON 파싱 헬퍼 함수들
    template<typename T>
    std::optional<T> getJsonValue(const nlohmann::json& j, const std::string& key);
};