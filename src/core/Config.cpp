#include "core/Config.hpp"
#include "core/Logger.hpp"
#include <fstream>
#include <filesystem>

bool Config::loadConfig(const std::filesystem::path& configPath) {
    try {
        std::ifstream file(configPath);
        if (!file.is_open()) {
            LOG_ERROR("Failed to open config file: {}", configPath.string());
            return false;
        }

        nlohmann::json j;
        file >> j;

        // WebRTC 설정 파싱
        if (j.contains("camera_id")) {
            webrtcConfig_.cameraId = j["camera_id"];
        }
        
        if (j.contains("server_ip")) {
            webrtcConfig_.serverIp = j["server_ip"];
        }
        
        if (j.contains("max_stream_cnt")) {
            webrtcConfig_.maxStreamCount = j["max_stream_cnt"];
        }
        
        if (j.contains("stream_base_port")) {
            webrtcConfig_.streamBasePort = j["stream_base_port"];
        }
        
        if (j.contains("comm_socket_port")) {
            webrtcConfig_.commSocketPort = j["comm_socket_port"];
        }
        
        if (j.contains("status_timer_interval")) {
            webrtcConfig_.statusTimerInterval = j["status_timer_interval"];
        }

        // 비디오 설정 파싱
        if (j.contains("device_cnt")) {
            int deviceCnt = j["device_cnt"];
            webrtcConfig_.videoConfigs.clear();
            
            for (int i = 0; i < deviceCnt; ++i) {
                VideoConfig videoConfig;
                
                std::string idx = std::to_string(i);
                if (j.contains("video_src_" + idx)) {
                    videoConfig.source = j["video_src_" + idx];
                }
                if (j.contains("video_enc_" + idx)) {
                    videoConfig.encoder = j["video_enc_" + idx];
                }
                if (j.contains("video_infer_" + idx)) {
                    videoConfig.inferConfig = j["video_infer_" + idx];
                }
                if (j.contains("bitrate_" + idx)) {
                    videoConfig.bitrate = j["bitrate_" + idx];
                }
                
                webrtcConfig_.videoConfigs.push_back(videoConfig);
            }
        }

        // 추가 설정들
        webrtcConfig_.deviceCnt = j.value("device_cnt", 1);
        webrtcConfig_.codecName = j.value("codec_name", "H264");
        webrtcConfig_.snapshotPath = j.value("snapshot_path", "/tmp/snapshots");
        webrtcConfig_.recordPath = j.value("record_path", "/home/nvidia/data");
        webrtcConfig_.recordDuration = j.value("record_duration", 60);
        webrtcConfig_.eventBufTime = j.value("event_buf_time", 10);
        webrtcConfig_.ttyName = j.value("tty_name", "");
        webrtcConfig_.ttyBaudrate = j.value("tty_baudrate", 38400);

        // 비디오 소스 배열 파싱
        for (int i = 0; i < webrtcConfig_.deviceCnt && i < 2; ++i) {
            std::string key = "video_src_" + std::to_string(i);
            if (j.contains(key)) {
                webrtcConfig_.videoSrc[i] = j[key];
            }
            
            key = "video_enc_" + std::to_string(i);
            if (j.contains(key)) {
                webrtcConfig_.videoEnc[i] = j[key];
            }
            
            key = "video_enc2_" + std::to_string(i);
            if (j.contains(key)) {
                webrtcConfig_.videoEnc2[i] = j[key];
            }
            
            key = "video_infer_" + std::to_string(i);
            if (j.contains(key)) {
                webrtcConfig_.videoInfer[i] = j[key];
            }
            
            key = "record_enc_" + std::to_string(i);
            if (j.contains(key)) {
                webrtcConfig_.recordEnc[i] = j[key];
            }
            
            key = "snapshot_enc_" + std::to_string(i);
            if (j.contains(key)) {
                webrtcConfig_.snapshotEnc[i] = j[key];
            }
        }

        LOG_INFO("Config loaded successfully from: {}", configPath.string());
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to load config: {}", e.what());
        return false;
    }
}

bool Config::loadDeviceSettings(const std::filesystem::path& settingsPath) {
    try {
        deviceSettingsPath_ = settingsPath;
        
        if (!std::filesystem::exists(settingsPath)) {
            LOG_WARNING("Device settings file not found: {}", settingsPath.string());
            // 기본값 사용
            return true;
        }

        std::ifstream file(settingsPath);
        if (!file.is_open()) {
            LOG_ERROR("Failed to open device settings: {}", settingsPath.string());
            return false;
        }

        nlohmann::json j;
        file >> j;

        // 디바이스 설정 파싱
        deviceSettings_.colorPalette = j.value("color_palette", 0);
        deviceSettings_.recordStatus = j.value("record_status", false);
        deviceSettings_.analysisStatus = j.value("analysis_status", false);
        deviceSettings_.autoPtzMoveSpeed = j.value("auto_ptz_move_speed", 0x08);
        deviceSettings_.ptzMoveSpeed = j.value("ptz_move_speed", 0x10);
        
        // 열화상 관련 설정
        deviceSettings_.tempApply = j.value("temp_apply", false);
        deviceSettings_.thresholdUnderTemp = j.value("threshold_under_temp", 15);
        deviceSettings_.thresholdUpperTemp = j.value("threshold_upper_temp", 50);
        deviceSettings_.tempDiffThreshold = j.value("temp_diff_threshold", 7);
        deviceSettings_.overTempTime = j.value("over_temp_time", 15);
        deviceSettings_.tempCorrection = j.value("temp_correction", 0);
        deviceSettings_.displayTemp = j.value("display_temp", true);
        deviceSettings_.enableEventNotify = j.value("enable_event_notify", true);

        // PTZ 프리셋
        if (j.contains("ptz_presets") && j["ptz_presets"].is_array()) {
            deviceSettings_.ptzPresets.clear();
            for (const auto& preset : j["ptz_presets"]) {
                deviceSettings_.ptzPresets.push_back(preset);
            }
        }

        LOG_INFO("Device settings loaded from: {}", settingsPath.string());
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to load device settings: {}", e.what());
        return false;
    }
}

bool Config::saveDeviceSettings() {
    try {
        if (deviceSettingsPath_.empty()) {
            LOG_ERROR("Device settings path not set");
            return false;
        }

        nlohmann::json j;
        
        // 디바이스 설정 저장
        j["color_palette"] = deviceSettings_.colorPalette;
        j["record_status"] = deviceSettings_.recordStatus;
        j["analysis_status"] = deviceSettings_.analysisStatus;
        j["auto_ptz_move_speed"] = deviceSettings_.autoPtzMoveSpeed;
        j["ptz_move_speed"] = deviceSettings_.ptzMoveSpeed;
        
        // 열화상 관련 설정
        j["temp_apply"] = deviceSettings_.tempApply;
        j["threshold_under_temp"] = deviceSettings_.thresholdUnderTemp;
        j["threshold_upper_temp"] = deviceSettings_.thresholdUpperTemp;
        j["temp_diff_threshold"] = deviceSettings_.tempDiffThreshold;
        j["over_temp_time"] = deviceSettings_.overTempTime;
        j["temp_correction"] = deviceSettings_.tempCorrection;
        j["display_temp"] = deviceSettings_.displayTemp;
        j["enable_event_notify"] = deviceSettings_.enableEventNotify;
        
        // PTZ 프리셋
        j["ptz_presets"] = deviceSettings_.ptzPresets;

        std::ofstream file(deviceSettingsPath_);
        if (!file.is_open()) {
            LOG_ERROR("Failed to open device settings for writing: {}", 
                     deviceSettingsPath_.string());
            return false;
        }

        file << j.dump(4);
        file.close();

        LOG_INFO("Device settings saved to: {}", deviceSettingsPath_.string());
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to save device settings: {}", e.what());
        return false;
    }
}

template<typename T>
std::optional<T> Config::getJsonValue(const nlohmann::json& j, const std::string& key) {
    if (j.contains(key)) {
        try {
            return j[key].get<T>();
        } catch (const std::exception& e) {
            LOG_WARNING("Failed to parse '{}': {}", key, e.what());
        }
    }
    return std::nullopt;
}