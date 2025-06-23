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
        webrtcConfig_.cameraId = j.value("camera_id", "");
        webrtcConfig_.serverIp = j.value("server_ip", "ws://localhost");
        webrtcConfig_.commSocketPort = j.value("comm_socket_port", 6000);
        webrtcConfig_.maxStreamCount = j.value("max_stream_cnt", 10);
        webrtcConfig_.streamBasePort = j.value("stream_base_port", 5000);
        webrtcConfig_.deviceCnt = j.value("device_cnt", 2);
        
        // 경로 설정
        webrtcConfig_.snapshotPath = j.value("snapshot_path", "/home/nvidia/webrtc");
        webrtcConfig_.recordPath = j.value("record_path", "/home/nvidia/data");
        webrtcConfig_.deviceSettingPath = j.value("device_setting_path", "/home/nvidia/webrtc/device_setting.json");
        
        // 녹화 설정
        webrtcConfig_.recordDuration = j.value("record_duration", 5);
        webrtcConfig_.recordEncIndex = j.value("record_enc_index", 1);
        webrtcConfig_.eventRecordEncIndex = j.value("event_record_enc_index", 0);
        webrtcConfig_.eventBufTime = j.value("event_buf_time", 15);
        
        // 서버 설정
        webrtcConfig_.eventUserId = j.value("event_user_id", "itechour");
        webrtcConfig_.eventUserPw = j.value("event_user_pw", "12341234");
        webrtcConfig_.eventServerIp = j.value("event_server_ip", "52.194.238.184");
        
        // 기타 설정
        webrtcConfig_.statusTimerInterval = j.value("status_timer_interval", 5000);
        webrtcConfig_.httpServicePort = j.value("http_service_port", "9617");
        
        // TTY 설정
        if (j.contains("tty")) {
            auto tty = j["tty"];
            webrtcConfig_.ttyName = tty.value("name", "/dev/ttyTHS0");
            webrtcConfig_.ttyBaudrate = tty.value("baudrate", 38400);
        }
        
        // 비디오 설정 파싱 (video0, video1)
        for (int i = 0; i < webrtcConfig_.deviceCnt && i < 2; ++i) {
            std::string videoKey = "video" + std::to_string(i);
            if (j.contains(videoKey)) {
                auto video = j[videoKey];
                webrtcConfig_.video[i].src = video.value("src", "");
                webrtcConfig_.video[i].record = video.value("record", "");
                webrtcConfig_.video[i].infer = video.value("infer", "");
                webrtcConfig_.video[i].enc = video.value("enc", "");
                webrtcConfig_.video[i].enc2 = video.value("enc2", "");
                webrtcConfig_.video[i].snapshot = video.value("snapshot", "");
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
        deviceSettings_.colorPalette = j.value("color_platte", 6);  // 오타 주의
        deviceSettings_.recordStatus = j.value("record_status", 1);
        deviceSettings_.analysisStatus = j.value("analysis_status", 1);
        
        // PTZ 설정
        deviceSettings_.autoPtzSeq = j.value("auto_ptz_seq", "0,1,2,3,4,5,6,7,8,9,FF,15");
        deviceSettings_.autoPtzMoveSpeed = j.value("auto_ptz_move_speed", 48);
        deviceSettings_.ptzMoveSpeed = j.value("ptz_move_speed", 48);
        
        // PTZ 프리셋
        if (j.contains("ptz_preset") && j["ptz_preset"].is_array()) {
            deviceSettings_.ptzPreset.clear();
            for (const auto& preset : j["ptz_preset"]) {
                deviceSettings_.ptzPreset.push_back(preset);
            }
        }
        
        if (j.contains("auto_ptz_preset") && j["auto_ptz_preset"].is_array()) {
            deviceSettings_.autoPtzPreset.clear();
            for (const auto& preset : j["auto_ptz_preset"]) {
                deviceSettings_.autoPtzPreset.push_back(preset);
            }
        }
        
        // 이벤트 설정
        deviceSettings_.enableEventNotify = j.value("enable_event_notify", 1);
        deviceSettings_.cameraDnMode = j.value("camera_dn_mode", 1);
        deviceSettings_.nvInterval = j.value("nv_interval", 2);
        
        // 옵티컬 플로우 설정
        deviceSettings_.optFlowThreshold = j.value("opt_flow_threshold", 11);
        deviceSettings_.optFlowApply = j.value("opt_flow_apply", 1);
        
        // ResNet50 설정
        deviceSettings_.resnet50Threshold = j.value("resnet50_threshold", 6);
        deviceSettings_.resnet50Apply = j.value("resnet50_apply", 0);
        
        // 임계값 설정
        deviceSettings_.normalThreshold = j.value("normal_threshold", 30);
        deviceSettings_.heatThreshold = j.value("heat_threshold", 101);
        deviceSettings_.flipThreshold = j.value("flip_threshold", 80);
        deviceSettings_.laborSignThreshold = j.value("labor_sign_threshold", 101);
        deviceSettings_.normalSittingThreshold = j.value("normal_sitting_threshold", 25);
        
        // 시간 설정
        deviceSettings_.heatTime = j.value("heat_time", 15);
        deviceSettings_.flipTime = j.value("flip_time", 15);
        deviceSettings_.laborSignTime = j.value("labor_sign_time", 15);
        deviceSettings_.overTempTime = j.value("over_temp_time", 15);
        
        // 온도 설정
        deviceSettings_.tempApply = j.value("temp_apply", 1);
        deviceSettings_.displayTemp = j.value("display_temp", 1);
        deviceSettings_.tempDiffThreshold = j.value("temp_diff_threshold", 12);
        deviceSettings_.tempCorrection = j.value("temp_correction", 8);
        deviceSettings_.thresholdUpperTemp = j.value("threshold_upper_temp", 35);
        deviceSettings_.thresholdUnderTemp = j.value("threshold_under_temp", 15);
        
        // 기타 설정
        deviceSettings_.cameraIndex = j.value("camera_index", 0);
        deviceSettings_.showNormalText = j.value("show_normal_text", 0);

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
        j["color_platte"] = deviceSettings_.colorPalette;  // 오타 유지
        j["record_status"] = deviceSettings_.recordStatus ? 1 : 0;
        j["analysis_status"] = deviceSettings_.analysisStatus ? 1 : 0;
        
        // PTZ 설정
        j["auto_ptz_seq"] = deviceSettings_.autoPtzSeq;
        j["auto_ptz_move_speed"] = deviceSettings_.autoPtzMoveSpeed;
        j["ptz_move_speed"] = deviceSettings_.ptzMoveSpeed;
        j["ptz_preset"] = deviceSettings_.ptzPreset;
        j["auto_ptz_preset"] = deviceSettings_.autoPtzPreset;
        
        // 이벤트 설정
        j["enable_event_notify"] = deviceSettings_.enableEventNotify ? 1 : 0;
        j["camera_dn_mode"] = deviceSettings_.cameraDnMode;
        j["nv_interval"] = deviceSettings_.nvInterval;
        
        // 옵티컬 플로우 설정
        j["opt_flow_threshold"] = deviceSettings_.optFlowThreshold;
        j["opt_flow_apply"] = deviceSettings_.optFlowApply ? 1 : 0;
        
        // ResNet50 설정
        j["resnet50_threshold"] = deviceSettings_.resnet50Threshold;
        j["resnet50_apply"] = deviceSettings_.resnet50Apply ? 1 : 0;
        
        // 임계값 설정
        j["normal_threshold"] = deviceSettings_.normalThreshold;
        j["heat_threshold"] = deviceSettings_.heatThreshold;
        j["flip_threshold"] = deviceSettings_.flipThreshold;
        j["labor_sign_threshold"] = deviceSettings_.laborSignThreshold;
        j["normal_sitting_threshold"] = deviceSettings_.normalSittingThreshold;
        
        // 시간 설정
        j["heat_time"] = deviceSettings_.heatTime;
        j["flip_time"] = deviceSettings_.flipTime;
        j["labor_sign_time"] = deviceSettings_.laborSignTime;
        j["over_temp_time"] = deviceSettings_.overTempTime;
        
        // 온도 설정
        j["temp_apply"] = deviceSettings_.tempApply ? 1 : 0;
        j["display_temp"] = deviceSettings_.displayTemp ? 1 : 0;
        j["temp_diff_threshold"] = deviceSettings_.tempDiffThreshold;
        j["temp_correction"] = deviceSettings_.tempCorrection;
        j["threshold_upper_temp"] = deviceSettings_.thresholdUpperTemp;
        j["threshold_under_temp"] = deviceSettings_.thresholdUnderTemp;
        
        // 기타 설정
        j["camera_index"] = deviceSettings_.cameraIndex;
        j["show_normal_text"] = deviceSettings_.showNormalText ? 1 : 0;

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