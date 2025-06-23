#pragma once

#include <memory>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <filesystem>
#include <functional>
#include <glib.h>
#include <gst/gst.h>
#include "utils/Singleton.hpp"
#include "video/EventRecorder.hpp"

// Forward declarations
class WebSocketClient;
class Pipeline;
class WebRTCManager;
class MessageHandler;
class SerialPort;
class EventRecorder;
class ThermalMonitor;
class SystemMonitor;
class FileWatcher;
class CommandExecutor;
class Config;

// 간단한 Timer 클래스 (Timer.hpp가 없는 경우)
class Timer {
public:
    using Callback = std::function<void()>;
    
    Timer() = default;
    ~Timer() { stop(); }
    
    void setTimeout(Callback callback, std::chrono::milliseconds delay) {
        stop();
        running_ = true;
        
        thread_ = std::thread([this, callback, delay]() {
            std::this_thread::sleep_for(delay);
            if (running_) {
                callback();
                running_ = false;
            }
        });
    }
    
    void setInterval(Callback callback, std::chrono::milliseconds interval) {
        stop();
        running_ = true;
        
        thread_ = std::thread([this, callback, interval]() {
            while (running_) {
                std::this_thread::sleep_for(interval);
                if (running_) {
                    callback();
                }
            }
        });
    }
    
    void stop() {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    
    bool isRunning() const { return running_; }
    
private:
    std::atomic<bool> running_{false};
    std::thread thread_;
};

class Application {
public:
    static Application& getInstance() {
        static Application instance;
        return instance;
    }

    // 생명 주기
    bool initialize(int argc, char* argv[]);
    void run();
    void shutdown();

    // 상태 관리
    enum class State {
        UNKNOWN = 0,
        INITIALIZING,
        INITIALIZED,
        CONNECTING,
        CONNECTED,
        REGISTERING,
        REGISTERED,
        RUNNING,
        SHUTTING_DOWN,
        ERROR
    };

    State getState() const { return state_.load(); }
    bool isRunning() const { return running_.load(); }

    // 컴포넌트 접근
    std::shared_ptr<Pipeline> getPipeline() { return pipeline_; }
    std::shared_ptr<WebRTCManager> getWebRTCManager() { return webrtcManager_; }

private:
    Application() = default;
    ~Application();

    // 초기화 단계들
    bool parseArguments(int argc, char* argv[]);
    bool loadConfigurations();
    bool initializeLogging();
    bool initializeGStreamer();
    bool initializeHardware();
    bool createPipeline();
    bool setupWebSocket();
    bool setupMonitoring();
    bool registerCommands();
    void restartRecording();
    void startRecordingForCamera(int cameraIndex, const std::string& filename);
    void stopRecordingForCamera(int cameraIndex);
    void scheduleNextMidnightRestart();

    // 이벤트 핸들러
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onWebSocketMessage(const std::string& message);
    
    // 모니터링 콜백
    void onSystemAlert(const std::string& alert);
    void onThermalAlert(int objectId, float temperature);
    void onConfigFileChanged(const std::filesystem::path& path);
    void onRecordingComplete(const EventRecorder::EventInfo& event, const std::string& filePath);

    // 주기적 작업
    void heartbeatThread();
    void sendCameraStatus();
    void checkAndReconnect();

    // PTZ 제어
    void processPtzCommand(const std::string& command);
    void initializePtzPosition();

    // 상태 관리
    void setState(State newState);
    void handleError(const std::string& error);

    // 비디오 처리
    void setupAnalysisProbes();
    GstPadProbeReturn processVideoFrame(int cameraIndex, GstBuffer* buffer);
    std::string encodeImageToBase64(const std::string& filePath);
    void applyDeviceSettings();

    // 멤버 변수들
    std::atomic<State> state_{State::UNKNOWN};
    std::atomic<bool> running_{false};
    
    // 핵심 컴포넌트들
    std::unique_ptr<WebSocketClient> wsClient_;
    std::shared_ptr<Pipeline> pipeline_;
    std::shared_ptr<WebRTCManager> webrtcManager_;
    std::unique_ptr<MessageHandler> messageHandler_;
    
    // 하드웨어 및 모니터링
    std::unique_ptr<ThermalMonitor> thermalMonitor_;
    std::unique_ptr<FileWatcher> fileWatcher_;
    
    // 스레드
    std::thread heartbeatThread_;
    
    // GLib 메인 루프
    GMainLoop* mainLoop_ = nullptr;
    
    // 설정
    std::string configPath_ = "config.json";
    std::string deviceSettingsPath_ = "device_settings.json";
    
    // 재연결 관리
    std::atomic<int> reconnectAttempts_{0};
    std::chrono::steady_clock::time_point lastReconnectTime_;
    
    // 타이머 관련 멤버 추가
    std::unique_ptr<Timer> recordingTimer_;
    std::unique_ptr<Timer> midnightTimer_;
    std::unique_ptr<Timer> restartTimer_;
    std::unordered_map<int, pid_t> recordingPids_;
    
    // 통계
    struct Statistics {
        std::atomic<uint64_t> messagesReceived{0};
        std::atomic<uint64_t> messagesSent{0};
        std::atomic<uint64_t> reconnectCount{0};
        std::chrono::steady_clock::time_point startTime;
    } stats_;
};