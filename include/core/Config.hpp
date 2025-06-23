#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <nlohmann/json.hpp>

class Config {
public:
    struct VideoConfig {
        std::string source;
        std::string encoder;
        std::string inferConfig;
        int bitrate = 0;
    };

    struct WebRTCConfig {
        std::string cameraId;
        std::string serverIp;
        int maxStreamCount = 10;
        int streamBasePort = 5000;
        int commSocketPort = 8080;
        int statusTimerInterval = 5000;
        std::vector<VideoConfig> videoConfigs;
        
        // 추가 멤버들
        int deviceCnt = 1;
        std::string codecName = "H264";
        std::string snapshotPath = "/tmp/snapshots";
        std::string recordPath = "/home/nvidia/data";
        int recordDuration = 60;
        int eventBufTime = 10;
        std::string ttyName;
        int ttyBaudrate = 38400;
        
        // 배열 형태의 설정들 (기존 코드 호환성)
        std::string videoSrc[2];
        std::string videoEnc[2];
        std::string videoEnc2[2];
        std::string videoInfer[2];
        std::string recordEnc[2];
        std::string snapshotEnc[2];
    };

    struct DeviceSettings {
        int colorPalette = 0;
        bool recordStatus = false;
        bool analysisStatus = false;
        int autoPtzMoveSpeed = 0x08;
        int ptzMoveSpeed = 0x10;
        // ... 더 많은 설정들
    };

    static Config& getInstance() {
        static Config instance;
        return instance;
    }

    bool loadConfig(const std::filesystem::path& configPath);
    bool loadDeviceSettings(const std::filesystem::path& settingsPath);
    bool saveDeviceSettings();

    const WebRTCConfig& getWebRTCConfig() const { return webrtcConfig_; }
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