#include "core/Application.hpp"
#include "core/Logger.hpp"
#include "core/Config.hpp"
#include "network/WebSocketClient.hpp"
#include "network/WebRTCManager.hpp"
#include "network/MessageHandler.hpp"
#include "network/SignalingProtocol.hpp"
#include "video/Pipeline.hpp"
#include "video/EventRecorder.hpp"
#include "hardware/SerialPort.hpp"
#include "monitoring/ThermalMonitor.hpp"
#include "monitoring/SystemMonitor.hpp"
#include "utils/FileWatcher.hpp"
#include "utils/CommandExecutor.hpp"
#include <gst/gst.h>
#include <glib.h>
#include <csignal>
#include <getopt.h>
#include <fstream>
#include <iomanip>
#include <sys/wait.h>
#include <gst/video/video.h>
#include <nvbufsurface.h>
#include <gstnvdsmeta.h>

Application::~Application() {
    shutdown();
}

bool Application::initialize(int argc, char* argv[]) {
    LOG_INFO("Initializing application...");
    setState(State::INITIALIZING);
    
    stats_.startTime = std::chrono::steady_clock::now();
    
    try {
        // 1. 명령줄 인자 파싱
        if (!parseArguments(argc, argv)) {
            LOG_ERROR("Failed to parse arguments");
            return false;
        }
        
        // 2. 로깅 초기화
        if (!initializeLogging()) {
            return false;
        }
        
        // 3. 설정 파일 로드
        if (!loadConfigurations()) {
            LOG_ERROR("Failed to load configurations");
            return false;
        }
        
        // 4. GStreamer 초기화
        if (!initializeGStreamer()) {
            LOG_ERROR("Failed to initialize GStreamer");
            return false;
        }
        
        // 5. 하드웨어 초기화
        if (!initializeHardware()) {
            LOG_ERROR("Failed to initialize hardware");
            return false;
        }
        
        // 6. 파이프라인 생성
        if (!createPipeline()) {
            LOG_ERROR("Failed to create pipeline");
            return false;
        }
        
        // 7. WebRTC 매니저 생성
        webrtcManager_ = std::make_shared<WebRTCManager>(pipeline_);
        
        // 8. 메시지 핸들러 생성
        messageHandler_ = std::make_unique<MessageHandler>(webrtcManager_);
        
        // 9. WebSocket 설정
        if (!setupWebSocket()) {
            LOG_ERROR("Failed to setup WebSocket");
            return false;
        }
        
        // 10. 모니터링 설정
        if (!setupMonitoring()) {
            LOG_ERROR("Failed to setup monitoring");
            return false;
        }
        
        // 11. 명령 등록
        if (!registerCommands()) {
            LOG_ERROR("Failed to register commands");
            return false;
        }
        
        // 12. 메인 루프 생성
        mainLoop_ = g_main_loop_new(nullptr, FALSE);
        if (!mainLoop_) {
            LOG_ERROR("Failed to create main loop");
            return false;
        }
        
        setState(State::INITIALIZED);
        LOG_INFO("Application initialized successfully");
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during initialization: {}", e.what());
        return false;
    }
}

bool Application::loadConfigurations() {
    // 설정 파일 로드
    if (!Config::getInstance().loadConfig(configPath_)) {
        LOG_ERROR("Failed to load config from: {}", configPath_);
        return false;
    }
    
    // 디바이스 설정 파일 경로 설정
    const auto& webrtcConfig = Config::getInstance().getWebRTCConfig();
    deviceSettingsPath_ = webrtcConfig.deviceSettingPath;
    
    // 디바이스 설정 로드
    if (!Config::getInstance().loadDeviceSettings(deviceSettingsPath_)) {
        LOG_WARNING("Failed to load device settings from: {}", deviceSettingsPath_);
        // 디바이스 설정은 기본값으로 진행 가능
    }
    
    LOG_INFO("Configuration loaded successfully");
    return true;
}

bool Application::parseArguments(int argc, char* argv[]) {
    const struct option longOptions[] = {
        {"config", required_argument, nullptr, 'c'},
        {"log-level", required_argument, nullptr, 'l'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "c:l:h", longOptions, nullptr)) != -1) {
        switch (opt) {
            case 'c':
                configPath_ = optarg;
                break;
                
            case 'l': {
                std::string level = optarg;
                if (level == "trace") Logger::getInstance().setLogLevel(LogLevel::TRACE);
                else if (level == "debug") Logger::getInstance().setLogLevel(LogLevel::DEBUG_LEVEL);
                else if (level == "info") Logger::getInstance().setLogLevel(LogLevel::INFO);
                else if (level == "warning") Logger::getInstance().setLogLevel(LogLevel::WARNING);
                else if (level == "error") Logger::getInstance().setLogLevel(LogLevel::ERROR);
                else if (level == "critical") Logger::getInstance().setLogLevel(LogLevel::CRITICAL);
                else {
                    std::cerr << "Invalid log level: " << level << std::endl;
                    return false;
                }
                break;
            }
            
            case 'h':
                std::cout << "Usage: " << argv[0] << " [options]\n"
                         << "Options:\n"
                         << "  -c, --config <file>     Configuration file path\n"
                         << "  -l, --log-level <level> Log level (trace|debug|info|warning|error|critical)\n"
                         << "  -h, --help              Show this help message\n";
                exit(EXIT_SUCCESS);
                
            default:
                return false;
        }
    }
    
    return true;
}

bool Application::initializeLogging() {
    // 로그 디렉토리 생성
    std::filesystem::create_directories("logs");
    
    // 날짜별 로그 파일
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << "logs/" << std::put_time(std::localtime(&time_t), "%Y-%m-%d") << "_webrtc_camera.log";
    
    Logger::getInstance().setLogFile(ss.str());
    
    LOG_INFO("Logging initialized to: {}", ss.str());
    return true;
}

bool Application::initializeGStreamer() {
    LOG_INFO("Initializing GStreamer...");
    
    // CUDA 초기화 (GStreamer보다 먼저)
    cudaError_t cudaStatus = cudaSetDevice(0);
    if (cudaStatus != cudaSuccess) {
        LOG_WARNING("Failed to initialize CUDA: {}", cudaGetErrorString(cudaStatus));
    } else {
        LOG_INFO("CUDA initialized successfully");
        
        // CUDA 컨텍스트 생성
        cudaFree(0);
    }
    
    // 환경 변수 설정 (CUDA 관련)
    setenv("CUDA_DEVICE_ORDER", "PCI_BUS_ID", 1);
    setenv("CUDA_VISIBLE_DEVICES", "0", 1);
    
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        LOG_ERROR("Failed to initialize GStreamer: {}", 
                 error ? error->message : "Unknown error");
        if (error) g_error_free(error);
        return false;
    }
    
    // 필요한 플러그인 확인
    const char* requiredPlugins[] = {
        "coreelements", "videoconvert", "videoscale", "videotestsrc",
        "x264", "vpx", "webrtc", "nice", "dtls", "srtp", "rtpmanager",
        "nvvideoconvert", "nvv4l2h264enc", "nvstreammux", "nvinfer"
    };
    
    GstRegistry* registry = gst_registry_get();
    for (const auto& pluginName : requiredPlugins) {
        GstPlugin* plugin = gst_registry_find_plugin(registry, pluginName);
        if (!plugin) {
            LOG_WARNING("GStreamer plugin not found: {} (optional)", pluginName);
        } else {
            gst_object_unref(plugin);
        }
    }
    
    // GStreamer 버전 정보
    guint major, minor, micro, nano;
    gst_version(&major, &minor, &micro, &nano);
    LOG_INFO("GStreamer version: {}.{}.{}.{}", major, minor, micro, nano);
    
    return true;
}

bool Application::initializeHardware() {
   LOG_INFO("Initializing hardware...");
   
   const auto& config = Config::getInstance().getWebRTCConfig();
   
   // Serial 포트 초기화 (PTZ 제어용)
   if (!config.ttyName.empty()) {
       SerialPort::Config serialConfig;
       serialConfig.portName = config.ttyName;
       serialConfig.baudRate = config.ttyBaudrate;
       
       if (!SerialPort::getInstance().open(serialConfig)) {
           LOG_WARNING("Failed to open serial port: {} (PTZ control disabled)", config.ttyName);
       } else {
           LOG_INFO("Serial port opened: {} @ {} baud", config.ttyName, config.ttyBaudrate);
           
           // PTZ 응답 처리 콜백
           SerialPort::getInstance().setDataCallback(
               [this](const std::vector<uint8_t>& data) {
                   // PTZ 응답 로깅
                   std::stringstream ss;
                   ss << "PTZ Response: ";
                   for (auto byte : data) {
                       ss << std::hex << std::setw(2) << std::setfill('0') 
                          << static_cast<int>(byte) << " ";
                   }
                   LOG_TRACE("{}", ss.str());
               }
           );
           
           // 초기 PTZ 위치로 이동
           initializePtzPosition();
       }
   }
   
   return true;
}

bool Application::createPipeline() {
    LOG_INFO("Creating pipeline...");
    
    const auto& config = Config::getInstance().getWebRTCConfig();
    
    Pipeline::PipelineConfig pipelineConfig;
    pipelineConfig.snapshotPath = config.snapshotPath;
    pipelineConfig.maxStreamCount = config.maxStreamCount;
    pipelineConfig.basePort = config.streamBasePort;
    pipelineConfig.webrtcConfig = config;  // 전체 config 전달
    
    // 스냅샷 디렉토리 생성
    std::filesystem::create_directories(config.snapshotPath);
    
    pipeline_ = std::make_shared<Pipeline>();
    if (!pipeline_->create(pipelineConfig)) {
        LOG_ERROR("Failed to create pipeline");
        return false;
    }
    
    // 비디오 분석 프로브 설정
    setupAnalysisProbes();
    
    // 파이프라인 시작
    if (!pipeline_->start()) {
        LOG_ERROR("Failed to start pipeline");
        return false;
    }
    
    // 녹화 디렉토리 생성
    std::filesystem::create_directories(config.recordPath);
    
    // 초기 녹화 시작
    restartRecording();
    
    // 5분마다 녹화 재시작을 위한 타이머 설정
    recordingTimer_ = std::make_unique<Timer>();
    recordingTimer_->setInterval([this]() {
        restartRecording();
    }, std::chrono::minutes(config.recordDuration));
    
    // 자정 재시작 스케줄러 설정
    scheduleNextMidnightRestart();
    
    LOG_INFO("Pipeline started successfully");
    return true;
}

void Application::scheduleNextMidnightRestart() {
    auto now = std::chrono::system_clock::now();
    auto midnight = std::chrono::system_clock::to_time_t(now);
    
    // 다음 자정 계산
    std::tm* tm = std::localtime(&midnight);
    tm->tm_hour = 0;
    tm->tm_min = 0;
    tm->tm_sec = 0;
    tm->tm_mday += 1;
    
    auto next_midnight = std::chrono::system_clock::from_time_t(std::mktime(tm));
    auto duration = next_midnight - now;
    
    // 자정 5분 전에 녹화 중지
    auto stop_duration = duration - std::chrono::minutes(5);
    
    LOG_INFO("Next midnight restart scheduled in {} hours", 
             std::chrono::duration_cast<std::chrono::hours>(duration).count());
    
    midnightTimer_ = std::make_unique<Timer>();
    midnightTimer_->setTimeout([this]() {
        LOG_INFO("Preparing for midnight restart - stopping recordings");
        
        // 모든 녹화 중지
        for (auto& [camIdx, pid] : recordingPids_) {
            stopRecordingForCamera(camIdx);
        }
        
        // 녹화 타이머 중지
        if (recordingTimer_) {
            recordingTimer_->stop();
        }
        
        // 5분 후 재시작
        restartTimer_ = std::make_unique<Timer>();
        restartTimer_->setTimeout([this]() {
            LOG_INFO("Midnight restart - shutting down application");
            shutdown();
            // systemd 서비스가 자동으로 재시작하도록 설정되어야 함
            exit(0);
        }, std::chrono::minutes(5));
        
    }, std::chrono::duration_cast<std::chrono::milliseconds>(stop_duration));
}

void Application::restartRecording() {
    LOG_INFO("Starting/Restarting recording for {}-minute interval", 
             Config::getInstance().getWebRTCConfig().recordDuration);
    
    const auto& config = Config::getInstance().getWebRTCConfig();
    
    // 현재 시간으로 파일명 생성
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    for (int i = 0; i < config.deviceCnt && i < 2; ++i) {
        // 이전 녹화 종료
        stopRecordingForCamera(i);
        
        std::stringstream filename;
        filename << config.recordPath << "/";
        filename << "cam" << i << "_";
        filename << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
        filename << ".mp4";
        
        // 새 녹화 시작
        startRecordingForCamera(i, filename.str());
    }
}


void Application::startRecordingForCamera(int cameraIndex, const std::string& filename) {
    const auto& config = Config::getInstance().getWebRTCConfig();
    
    // 녹화 포트 (config의 record 설정에서 지정된 포트)
    int recordPort = 7000 + cameraIndex;
    
    // 녹화 시간 계산 (분 단위를 초로 변환)
    int durationSeconds = config.recordDuration * 60;
    
    // FFmpeg 명령 구성
    std::stringstream cmd;
    cmd << "ffmpeg -y ";
    cmd << "-i udp://127.0.0.1:" << recordPort << " ";
    cmd << "-c copy ";
    cmd << "-f mp4 ";
    cmd << "-movflags +faststart ";
    cmd << "-t " << durationSeconds << " ";  // 녹화 시간 설정
    cmd << filename << " ";
    cmd << "2>/dev/null";
    
    LOG_DEBUG("Recording command: {}", cmd.str());
    
    // 프로세스 실행
    pid_t pid = fork();
    if (pid == 0) {
        // 자식 프로세스
        execl("/bin/sh", "sh", "-c", cmd.str().c_str(), nullptr);
        _exit(1);
    } else if (pid > 0) {
        // 부모 프로세스
        recordingPids_[cameraIndex] = pid;
        LOG_INFO("Started recording for camera {} to {} (PID: {})", 
                 cameraIndex, filename, pid);
    } else {
        LOG_ERROR("Failed to fork recording process for camera {}", cameraIndex);
    }
}

void Application::stopRecordingForCamera(int cameraIndex) {
    auto it = recordingPids_.find(cameraIndex);
    if (it != recordingPids_.end() && it->second > 0) {
        LOG_INFO("Stopping recording for camera {} (PID: {})", cameraIndex, it->second);
        
        // SIGTERM 전송
        kill(it->second, SIGTERM);
        
        // 프로세스 종료 대기 (최대 5초)
        int status;
        int waitTime = 0;
        while (waitTime < 5000) {
            pid_t result = ::waitpid(it->second, &status, WNOHANG);
            if (result == it->second) {
                LOG_INFO("Recording process {} terminated successfully", it->second);
                break;
            } else if (result == -1) {
                LOG_ERROR("waitpid failed for PID {}: {}", it->second, strerror(errno));
                break;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            waitTime += 100;
        }
        
        // 타임아웃 시 SIGKILL
        if (waitTime >= 5000) {
            LOG_WARNING("Recording process {} did not terminate, sending SIGKILL", it->second);
            kill(it->second, SIGKILL);
            ::waitpid(it->second, &status, 0);
        }
        
        recordingPids_.erase(it);
    }
}

bool Application::setupWebSocket() {
   LOG_INFO("Setting up WebSocket connection...");
   
   const auto& config = Config::getInstance().getWebRTCConfig();
   
   wsClient_ = std::make_unique<WebSocketClient>();
   
   // 콜백 설정
   wsClient_->setConnectedCallback([this]() { 
       LOG_INFO("WebSocket connected callback");
       onWebSocketConnected(); 
   });
   
   wsClient_->setDisconnectedCallback([this]() { 
       LOG_WARNING("WebSocket disconnected callback");
       onWebSocketDisconnected(); 
   });
   
   wsClient_->setMessageCallback([this](const std::string& msg) { 
       stats_.messagesReceived++;
       onWebSocketMessage(msg); 
   });
   
   // 메시지 핸들러에 전송 콜백 설정
   messageHandler_->setSendMessageCallback(
       [this](const std::string& msg) {
           if (wsClient_ && wsClient_->isConnected()) {
               wsClient_->sendText(msg);
               stats_.messagesSent++;
           } else {
               LOG_WARNING("Cannot send message - WebSocket not connected");
           }
       }
   );
   
   // URL 생성
   std::string wsUrl = config.serverIp + "/signaling/" + config.cameraId + 
                      "/?token=test&peerType=camera";
   
   LOG_INFO("Connecting to WebSocket: {}", wsUrl);
   
   // 연결 시도
   setState(State::CONNECTING);
   if (!wsClient_->connect(wsUrl)) {
       LOG_ERROR("Failed to initiate WebSocket connection");
       setState(State::ERROR);
       // 연결 실패해도 계속 실행 (재연결 시도)
   }
   
   return true;  // 초기 연결 실패해도 true 반환
}

bool Application::setupMonitoring() {
   LOG_INFO("Setting up monitoring...");
   
   // 시스템 모니터 시작
   SystemMonitor::getInstance().setAlertCallback(
       [this](const std::string& alert) { onSystemAlert(alert); }
   );
   
   SystemMonitor::AlertThresholds thresholds;
   thresholds.maxCpuTemp = 85;
   thresholds.maxGpuTemp = 85;
   thresholds.maxMemoryPercent = 90;
   thresholds.maxStoragePercent = 95;
   
   SystemMonitor::getInstance().setAlertThresholds(thresholds);
   SystemMonitor::getInstance().start(std::chrono::seconds(5));
   
   // 열화상 모니터 설정
   const auto& deviceSettings = Config::getInstance().getDeviceSettings();
   if (deviceSettings.tempApply && Config::getInstance().getWebRTCConfig().deviceCnt > 1) {
       thermalMonitor_ = std::make_unique<ThermalMonitor>();
       
       ThermalMonitor::ThermalConfig thermalConfig;
       thermalConfig.lowerThreshold = deviceSettings.thresholdUnderTemp;
       thermalConfig.upperThreshold = deviceSettings.thresholdUpperTemp;
       thermalConfig.tempDiffThreshold = deviceSettings.tempDiffThreshold;
       thermalConfig.overTempDuration = deviceSettings.overTempTime;
       thermalConfig.tempCorrection = deviceSettings.tempCorrection;
       thermalConfig.enableTempDisplay = deviceSettings.displayTemp;
       thermalConfig.enableTempNotification = deviceSettings.enableEventNotify;
       
       thermalMonitor_->setConfig(thermalConfig);
       thermalMonitor_->setOverTempCallback(
           [this](int objId, float temp) { onThermalAlert(objId, temp); }
       );
       
       LOG_INFO("Thermal monitoring enabled");
   }
   
   // 파일 감시자 설정
   fileWatcher_ = std::make_unique<FileWatcher>();
   
   // 설정 파일 감시
   fileWatcher_->watch(configPath_, 
       [this](const std::filesystem::path& path, bool exists) {
           if (exists) {
               LOG_INFO("Config file changed: {}", path.string());
               onConfigFileChanged(path);
           }
       }
   );
   
   fileWatcher_->watch(deviceSettingsPath_,
       [this](const std::filesystem::path& path, bool exists) {
           if (exists) {
               LOG_INFO("Device settings changed: {}", path.string());
               onConfigFileChanged(path);
           }
       }
   );
   
   fileWatcher_->start(std::chrono::milliseconds(1000));
   
   // 이벤트 레코더 초기화
   const auto& config = Config::getInstance().getWebRTCConfig();
   EventRecorder::Config recorderConfig;
   recorderConfig.recordPath = config.recordPath;
   recorderConfig.recordDuration = config.recordDuration;
   recorderConfig.preEventBuffer = config.eventBufTime;
   
   // 녹화 디렉토리 생성
   std::filesystem::create_directories(config.recordPath);
   
   EventRecorder::getInstance().initialize(recorderConfig);
   EventRecorder::getInstance().setCompletionCallback(
       [this](const auto& event, const auto& path) { 
           onRecordingComplete(event, path); 
       }
   );
   
   return true;
}

bool Application::registerCommands() {
   LOG_INFO("Registering allowed commands...");
   
   auto& executor = CommandExecutor::getInstance();
   
   // 시스템 정보 명령들
   executor.registerAllowedCommand("uptime", "uptime");
   executor.registerAllowedCommand("df", "df -h");
   executor.registerAllowedCommand("free", "free -h");
   executor.registerAllowedCommand("ps", "ps aux | grep -E '(gstream|webrtc)'");
   executor.registerAllowedCommand("netstat", "netstat -tuln");
   //executor.registerAllowedCommand("cpuinfo", "cat /proc/cpuinfo | head -20");
   
   // Jetson 관련
   //executor.registerAllowedCommand("tegrastats", "timeout 5 tegrastats");
   //executor.registerAllowedCommand("jetson_clocks", "jetson_clocks --show");
   
   // NVIDIA 관련
   //executor.registerAllowedCommand("nvidia-smi", "nvidia-smi");
   
   return true;
}

void Application::run() {
   if (running_) {
       LOG_WARNING("Application already running");
       return;
   }
   
   running_ = true;
   
   LOG_INFO("Starting application main loop");
   setState(State::RUNNING);
   
   // Heartbeat 스레드 시작
   heartbeatThread_ = std::thread(&Application::heartbeatThread, this);
   
   // 메인 루프 실행
   if (mainLoop_) {
       g_main_loop_run(mainLoop_);
   } else {
       // GLib 메인 루프가 없는 경우 직접 루프 실행
       LOG_WARNING("No GLib main loop, running manual loop");
       while (running_) {
           std::this_thread::sleep_for(std::chrono::milliseconds(100));
       }
   }
   
   LOG_INFO("Main loop ended");
}


void Application::shutdown() {
    if (!running_.exchange(false)) {
        return; // 이미 종료 중
    }
    
    LOG_INFO("Shutting down application");
    setState(State::SHUTTING_DOWN);
    
    // 1. 모든 녹화 프로세스 먼저 종료
    for (auto& [camIdx, pid] : recordingPids_) {
        stopRecordingForCamera(camIdx);
    }
    recordingPids_.clear();
    
    // 2. 타이머 중지
    if (recordingTimer_) {
        recordingTimer_->stop();
        recordingTimer_.reset();
    }
    if (midnightTimer_) {
        midnightTimer_->stop();
        midnightTimer_.reset();
    }
    if (restartTimer_) {
        restartTimer_->stop();
        restartTimer_.reset();
    }
    
    // 3. 스레드 종료
    if (heartbeatThread_.joinable()) {
        heartbeatThread_.join();
    }
    
    // 4. WebSocket 연결 종료
    if (wsClient_) {
        wsClient_->disconnect();
        wsClient_.reset();
    }
    
    // 5. WebRTC 연결 정리
    if (webrtcManager_) {
        webrtcManager_->removeAllPeers();
        webrtcManager_.reset();
    }
    
    // 6. 메시지 핸들러 정리
    if (messageHandler_) {
        messageHandler_.reset();
    }
    
    // 7. 파이프라인 중지 (CUDA 리소스 해제 전에)
    if (pipeline_) {
        pipeline_->stop();
        
        // 파이프라인 상태가 완전히 NULL이 될 때까지 대기
        int waitCount = 0;
        while (pipeline_->getState() != GST_STATE_NULL && waitCount < 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            waitCount++;
        }
        
        pipeline_.reset();
    }
    
    // 8. 모니터링 중지
    SystemMonitor::getInstance().stop();
    EventRecorder::getInstance().shutdown();
    
    if (thermalMonitor_) {
        thermalMonitor_.reset();
    }
    
    if (fileWatcher_) {
        fileWatcher_->stop();
        fileWatcher_.reset();
    }
    
    // 9. 하드웨어 정리
    SerialPort::getInstance().close();
    
    // 10. 메인 루프 종료
    if (mainLoop_) {
        g_main_loop_quit(mainLoop_);
        
        // 메인 루프가 완전히 종료될 때까지 대기
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        g_main_loop_unref(mainLoop_);
        mainLoop_ = nullptr;
    }
    
    // 11. CUDA 컨텍스트 정리 (마지막에)
    cudaDeviceReset();
    
    // 통계 출력
    auto uptime = std::chrono::steady_clock::now() - stats_.startTime;
    auto hours = std::chrono::duration_cast<std::chrono::hours>(uptime);
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(uptime - hours);
    
    LOG_INFO("Application statistics:");
    LOG_INFO("  Uptime: {}h {}m", hours.count(), minutes.count());
    LOG_INFO("  Messages sent: {}", stats_.messagesSent.load());
    LOG_INFO("  Messages received: {}", stats_.messagesReceived.load());
    LOG_INFO("  Reconnect count: {}", stats_.reconnectCount.load());
    
    setState(State::UNKNOWN);
    LOG_INFO("Application shutdown complete");
}

// WebSocket 이벤트 핸들러
void Application::onWebSocketConnected() {
    LOG_INFO("WebSocket connected - registering with server");
    setState(State::CONNECTED);
    reconnectAttempts_ = 0;
    
    // 서버에 등록
    const auto& config = Config::getInstance().getWebRTCConfig();
    messageHandler_->sendRegistration(config.cameraId);
    
    // 서버가 등록 응답을 보내지 않으므로 바로 REGISTERED 상태로
    setState(State::REGISTERED);
    LOG_INFO("Transitioned to REGISTERED state");
    
    // 첫 카메라 상태 전송 (약간의 지연 후)
    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        sendCameraStatus();
    }).detach();
}

void Application::onWebSocketDisconnected() {
   LOG_WARNING("WebSocket disconnected");
   setState(State::CONNECTING);
   
   // 모든 WebRTC 연결 정리
   if (webrtcManager_) {
       webrtcManager_->removeAllPeers();
   }
   
   // 재연결은 heartbeat 스레드에서 처리
}

void Application::onWebSocketMessage(const std::string& message) {
    LOG_TRACE("WebSocket message received: {}", 
            message.length() > 100 ? message.substr(0, 100) + "..." : message);
    
    // camstatus_reply를 받으면 등록이 성공한 것으로 간주
    try {
        auto j = nlohmann::json::parse(message);
        if (j.contains("action")) {
            std::string action = j["action"];
            
            // camstatus_reply를 받으면 확실히 등록된 것
            if (action == "camstatus_reply") {
                if (getState() != State::REGISTERED && getState() != State::RUNNING) {
                    setState(State::REGISTERED);
                    LOG_INFO("Confirmed registration via camstatus_reply");
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_TRACE("Message parsing failed: {}", e.what());
    }
    
    // 메시지 핸들러로 전달
    if (messageHandler_) {
        messageHandler_->handleMessage(message);
    }
}

// 주기적 작업
void Application::heartbeatThread() {
   const auto& config = Config::getInstance().getWebRTCConfig();
   auto interval = std::chrono::milliseconds(config.statusTimerInterval);
   
   // 초기 대기 (파이프라인 안정화)
   std::this_thread::sleep_for(std::chrono::seconds(3));
   
   while (running_) {
       try {
           // WebSocket 연결 상태 확인 및 재연결
           if (!wsClient_ || !wsClient_->isConnected()) {
               checkAndReconnect();
           } else {
               // 카메라 상태 전송
               LOG_DEBUG("State : {}, WebSocket connected: {}",
                         getState(), wsClient_->isConnected());
               if (getState() == State::REGISTERED || getState() == State::RUNNING) {
                   sendCameraStatus();
               }
           }
           
           // 디바이스 설정 적용 (변경된 경우)
           static auto lastSettingsCheck = std::chrono::steady_clock::now();
           auto now = std::chrono::steady_clock::now();
           if (now - lastSettingsCheck > std::chrono::seconds(5)) {
               applyDeviceSettings();
               lastSettingsCheck = now;
           }
           
       } catch (const std::exception& e) {
           LOG_ERROR("Exception in heartbeat thread: {}", e.what());
       }
       
       std::this_thread::sleep_for(interval);
   }
}

void Application::sendCameraStatus() {
   try {
       const auto& config = Config::getInstance().getWebRTCConfig();
       const auto& deviceSettings = Config::getInstance().getDeviceSettings();
       auto sysStatus = SystemMonitor::getInstance().getCurrentStatus();

       LOG_DEBUG("WebSocket connected, active peers: {}", 
              webrtcManager_ ? webrtcManager_->getPeerCount() : 0);
       
       // 스냅샷 이미지를 Base64로 인코딩
       std::string rgbSnapshot = encodeImageToBase64(
           config.snapshotPath + "/cam0_snapshot.jpg");
       std::string thermalSnapshot;
       
       if (config.deviceCnt > 1) {
           thermalSnapshot = encodeImageToBase64(
               config.snapshotPath + "/cam1_snapshot.jpg");
       }
       
       Signaling::CameraStatusMessage status;
       status.recordStatus = deviceSettings.recordStatus ? "On" : "Off";
       status.recordUsage = sysStatus.storageUsagePercent;
       status.cpuTemp = sysStatus.cpuTemp;
       status.gpuTemp = sysStatus.gpuTemp;
       status.rgbSnapshot = rgbSnapshot;
       status.thermalSnapshot = thermalSnapshot;
       
       messageHandler_->sendCameraStatus(status);
       
   } catch (const std::exception& e) {
       LOG_ERROR("Failed to send camera status: {}", e.what());
   }
}

void Application::checkAndReconnect() {
   if (!wsClient_ || wsClient_->isConnected()) {
       return;  // 이미 연결되어 있으면 재연결 불필요
   }
   
   auto now = std::chrono::steady_clock::now();
   auto timeSinceLastAttempt = std::chrono::duration_cast<std::chrono::seconds>(
       now - lastReconnectTime_).count();
   
   // 재연결 백오프: 1초 -> 2초 -> 4초 -> ... -> 최대 60초
   int backoffTime = std::min(60, (1 << reconnectAttempts_));
   
   if (timeSinceLastAttempt >= backoffTime) {
       LOG_INFO("Attempting reconnection #{} (backoff: {}s)", 
               reconnectAttempts_ + 1, backoffTime);
       
       const auto& config = Config::getInstance().getWebRTCConfig();
       std::string wsUrl = config.serverIp + "/signaling/" + config.cameraId + 
                          "/?token=test&peerType=camera";
       
       setState(State::CONNECTING);
       lastReconnectTime_ = now;
       
       if (wsClient_->connect(wsUrl)) {
           // 연결 성공은 콜백에서 처리
           LOG_INFO("WebSocket connection initiated");
       } else {
           reconnectAttempts_++;
           stats_.reconnectCount++;
           LOG_ERROR("Failed to initiate reconnection");
       }
   }
}

// 분석 프로브 설정
void Application::setupAnalysisProbes() {
    const auto& config = Config::getInstance().getWebRTCConfig();
    
    for (int i = 0; i < config.deviceCnt; ++i) {
        std::string osdName = "nvosd_" + std::to_string(i + 1);
        
        // OSD 엘리먼트가 있는 경우에만 프로브 추가
        if (pipeline_->getElement(osdName)) {
            pipeline_->addProbe(osdName, "sink", GST_PAD_PROBE_TYPE_BUFFER,
                [this, i](GstPad* /*pad*/, GstPadProbeInfo* info) {  // 미사용 매개변수 주석 처리
                    return processVideoFrame(i, GST_PAD_PROBE_INFO_BUFFER(info));
                }
            );
            LOG_INFO("Added analysis probe for camera {}", i);
        }
    }
}

// 비디오 프레임 처리
GstPadProbeReturn Application::processVideoFrame(int cameraIndex, GstBuffer* buffer) {
    // 여기서 실제 비디오 분석을 수행
    static uint64_t frameCount[2] = {0, 0};
    frameCount[cameraIndex]++;

    // 10초마다 프레임 카운트 로그
    if (frameCount[cameraIndex] % 300 == 0) {
        LOG_INFO("Camera {} processed {} frames", cameraIndex, frameCount[cameraIndex]);
    }

    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buffer);

    if (!batch_meta) {
        LOG_ERROR("Failed to get batch meta from buffer");
        return GST_PAD_PROBE_DROP;
    }

    // 메타데이터를 순회하며 분석
    for (NvDsMetaList *l_frame = batch_meta->frame_meta_list; l_frame != nullptr; l_frame = l_frame->next) {
        NvDsFrameMeta *frame_meta = reinterpret_cast<NvDsFrameMeta *>(l_frame->data);
        if (!frame_meta) {
            LOG_ERROR("Frame meta is null");
            continue;
        }  

        // 프레임 메타에서 객체 메타를 순회
        for (NvDsMetaList *l_obj = frame_meta->obj_meta_list; l_obj != nullptr; l_obj = l_obj->next) {
            NvDsObjectMeta *obj_meta = reinterpret_cast<NvDsObjectMeta *>(l_obj->data);
            if (!obj_meta) {
                LOG_ERROR("Object meta is null");
                continue;
            }
        }
    }

    return GST_PAD_PROBE_OK;
}

// 이미지를 Base64로 인코딩
std::string Application::encodeImageToBase64(const std::string& filePath) {
   try {
       std::ifstream file(filePath, std::ios::binary | std::ios::ate);
       if (!file.is_open()) {
           LOG_WARNING("Failed to open image file: {}", filePath);
           return "";
       }
       
       auto size = file.tellg();
       if (size <= 0 || size > 10 * 1024 * 1024) { // 10MB 제한
           LOG_WARNING("Invalid image file size: {}", size);
           return "";
       }
       
       file.seekg(0, std::ios::beg);
       std::vector<char> buffer(size);
       if (!file.read(buffer.data(), size)) {
           LOG_WARNING("Failed to read image file: {}", filePath);
           return "";
       }
       
       gchar* base64 = g_base64_encode(reinterpret_cast<const guchar*>(buffer.data()), size);
       std::string result(base64);
       g_free(base64);
       
       return result;
       
   } catch (const std::exception& e) {
       LOG_ERROR("Exception encoding image: {}", e.what());
       return "";
   }
}

// PTZ 초기화
void Application::initializePtzPosition() {
    const auto& settings = Config::getInstance().getDeviceSettings();
    
    // 기본 위치가 설정되어 있으면 이동 (ptzPresets -> ptzPreset으로 수정)
    if (settings.ptzPreset.size() > 0 && !settings.ptzPreset[0].empty()) {
        LOG_INFO("Moving to initial PTZ position");
        processPtzCommand("MOVE_PRESET:0");
    }
}

// PTZ 명령 처리
void Application::processPtzCommand(const std::string& command) {
    if (!SerialPort::getInstance().isOpen()) {
        LOG_WARNING("Cannot process PTZ command - serial port not open");
        return;
    }
    
    // 명령 파싱 및 처리
    if (command.find("MOVE_") == 0) {
        // 이동 명령
        std::vector<uint8_t> ptzData;
        // ... PTZ 프로토콜에 따른 데이터 생성
        SerialPort::getInstance().send(ptzData);
    }
}

// 디바이스 설정 적용
void Application::applyDeviceSettings() {
   static auto lastModified = std::filesystem::file_time_type::min();
   
   try {
       auto currentModified = std::filesystem::last_write_time(deviceSettingsPath_);
       if (currentModified <= lastModified) {
           return; // 변경 없음
       }
       
       lastModified = currentModified;
       
       // 설정 재로드
       if (!Config::getInstance().loadDeviceSettings(deviceSettingsPath_)) {
           LOG_ERROR("Failed to reload device settings");
           return;
       }
       
       const auto& settings = Config::getInstance().getDeviceSettings();
       
       // 녹화 상태 변경
       if (settings.recordStatus) {
           EventRecorder::getInstance().startManualRecording(0);
       } else {
           EventRecorder::getInstance().stopManualRecording(0);
       }
       
       // 분석 상태 변경
       // TODO: 파이프라인에 분석 ON/OFF 적용
       
       // 열화상 설정 업데이트
       if (thermalMonitor_) {
           ThermalMonitor::ThermalConfig thermalConfig;
           thermalConfig.lowerThreshold = settings.thresholdUnderTemp;
           thermalConfig.upperThreshold = settings.thresholdUpperTemp;
           thermalConfig.tempDiffThreshold = settings.tempDiffThreshold;
           thermalConfig.overTempDuration = settings.overTempTime;
           thermalConfig.tempCorrection = settings.tempCorrection;
           thermalConfig.enableTempDisplay = settings.displayTemp;
           
           thermalMonitor_->setConfig(thermalConfig);
       }
       
       LOG_DEBUG("Device settings applied");
       
   } catch (const std::exception& e) {
       LOG_ERROR("Failed to apply device settings: {}", e.what());
   }
}

// 모니터링 콜백들
void Application::onSystemAlert(const std::string& alert) {
   LOG_WARNING("System alert: {}", alert);
   
   // 심각한 경우 이벤트 트리거
   if (alert.find("critical") != std::string::npos ||
       alert.find("exceeded") != std::string::npos) {
       
       // 시스템 이벤트로 녹화
       EventRecorder::getInstance().triggerEvent(
           EventType::MANUAL, 
           0,
           "System alert: " + alert
       );
   }
}

void Application::onThermalAlert(int objectId, float temperature) {
   LOG_WARNING("Thermal alert - Object {}: {}°C", objectId, temperature);
   
   // 온도 이벤트 녹화
   if (Config::getInstance().getDeviceSettings().enableEventNotify) {
       EventRecorder::getInstance().triggerEvent(
           EventType::OVER_TEMP,
           1, // Thermal camera
           "Object " + std::to_string(objectId) + " temperature: " + 
           std::to_string(static_cast<int>(temperature)) + "°C"
       );
   }
}

void Application::onConfigFileChanged(const std::filesystem::path& path) {
   LOG_INFO("Configuration file changed: {}", path.string());
   
   // 일부 설정은 재시작 없이 적용 가능
   if (path == deviceSettingsPath_) {
       // 디바이스 설정은 applyDeviceSettings()에서 처리
       LOG_INFO("Device settings will be applied on next check");
   } else if (path == configPath_) {
       LOG_WARNING("Main configuration changed - restart required");
       // TODO: 재시작이 필요한 설정 변경 알림
   }
}

void Application::onRecordingComplete(const EventRecorder::EventInfo& event, 
                                    const std::string& filePath) {
   LOG_INFO("Recording complete: {} - {}", filePath, event.description);
   
   // 녹화 완료 알림을 서버로 전송
   // TODO: 서버 API에 맞춰 구현
}

void Application::setState(State newState) {
   State oldState = state_.exchange(newState);
   if (oldState != newState) {
       LOG_INFO("Application state changed: {} -> {}", 
               static_cast<int>(oldState), 
               static_cast<int>(newState));
       
       // 상태 변경에 따른 추가 작업
       if (newState == State::ERROR) {
           // 에러 상태 처리
           LOG_ERROR("Application entered ERROR state");
       }
   }
}

void Application::handleError(const std::string& error) {
   LOG_ERROR("Application error: {}", error);
   setState(State::ERROR);
   
   // 에러 복구 시도
   if (running_) {
       // 재연결 등의 복구 로직
       checkAndReconnect();
   }
}