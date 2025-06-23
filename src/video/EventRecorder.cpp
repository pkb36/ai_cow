#include "video/EventRecorder.hpp"
#include "core/Logger.hpp"
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <sys/wait.h>
#include <unistd.h>

EventRecorder::~EventRecorder() {
    shutdown();
}

bool EventRecorder::initialize(const Config& config) {
    config_ = config;
    
    // 녹화 디렉토리 생성
    try {
        std::filesystem::create_directories(config.recordPath);
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to create recording directory: {}", e.what());
        return false;
    }
    
    // 녹화 스레드 시작
    running_ = true;
    recordingThread_ = std::thread(&EventRecorder::recordingThread, this);
    
    LOG_INFO("EventRecorder initialized with path: {}", config.recordPath);
    return true;
}

void EventRecorder::shutdown() {
    if (!running_) {
        return;
    }
    
    LOG_INFO("Shutting down EventRecorder");
    
    running_ = false;
    queueCv_.notify_all();
    
    if (recordingThread_.joinable()) {
        recordingThread_.join();
    }
    
    // 진행 중인 녹화 종료
    {
        std::lock_guard<std::mutex> lock(recordingsMutex_);
        for (const auto& [cameraIndex, pid] : activeRecordings_) {
            LOG_WARNING("Terminating active recording for camera {}", cameraIndex);
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
        activeRecordings_.clear();
    }
}

bool EventRecorder::triggerEvent(EventType type, int cameraIndex, const std::string& description) {
    if (!running_) {
        LOG_ERROR("EventRecorder not running");
        return false;
    }
    
    // 동시 녹화 제한 확인
    if (getActiveRecordingCount() >= static_cast<size_t>(config_.maxConcurrentRecordings)) {
        LOG_WARNING("Maximum concurrent recordings reached");
        return false;
    }
    
    EventInfo event;
    event.type = type;
    event.cameraIndex = cameraIndex;
    event.timestamp = std::chrono::steady_clock::now();
    event.description = description;
    
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        eventQueue_.push(event);
        
        // 최근 이벤트 기록
        std::lock_guard<std::mutex> eventLock(eventsMutex_);
        recentEvents_.push_back(event);
        if (recentEvents_.size() > 100) {
            recentEvents_.erase(recentEvents_.begin());
        }
    }
    
    queueCv_.notify_one();
    
    LOG_INFO("Event triggered: type={}, camera={}, description={}", 
             static_cast<int>(type), cameraIndex, description);
    
    return true;
}

bool EventRecorder::startManualRecording(int cameraIndex) {
    return triggerEvent(EventType::MANUAL, cameraIndex, "Manual recording");
}

bool EventRecorder::stopManualRecording(int cameraIndex) {
    std::lock_guard<std::mutex> lock(recordingsMutex_);
    
    auto it = activeRecordings_.find(cameraIndex);
    if (it != activeRecordings_.end()) {
        LOG_INFO("Stopping manual recording for camera {}", cameraIndex);
        kill(it->second, SIGTERM);
        waitpid(it->second, nullptr, 0);
        activeRecordings_.erase(it);
        return true;
    }
    
    return false;
}

bool EventRecorder::isRecording(int cameraIndex) const {
    std::lock_guard<std::mutex> lock(recordingsMutex_);
    return activeRecordings_.find(cameraIndex) != activeRecordings_.end();
}

size_t EventRecorder::getActiveRecordingCount() const {
    std::lock_guard<std::mutex> lock(recordingsMutex_);
    return activeRecordings_.size();
}

std::vector<EventRecorder::EventInfo> EventRecorder::getRecentEvents(size_t count) const {
    std::lock_guard<std::mutex> lock(eventsMutex_);
    
    std::vector<EventInfo> result;
    size_t startIdx = recentEvents_.size() > count ? recentEvents_.size() - count : 0;
    
    for (size_t i = startIdx; i < recentEvents_.size(); ++i) {
        result.push_back(recentEvents_[i]);
    }
    
    return result;
}

void EventRecorder::recordingThread() {
    while (running_) {
        std::unique_lock<std::mutex> lock(queueMutex_);
        
        queueCv_.wait(lock, [this] { 
            return !eventQueue_.empty() || !running_; 
        });
        
        if (!running_) {
            break;
        }
        
        if (!eventQueue_.empty()) {
            EventInfo event = eventQueue_.front();
            eventQueue_.pop();
            lock.unlock();
            
            processEvent(event);
        }
    }
}

void EventRecorder::processEvent(const EventInfo& event) {
    // 이미 녹화 중인지 확인
    if (isRecording(event.cameraIndex)) {
        LOG_WARNING("Camera {} is already recording", event.cameraIndex);
        return;
    }
    
    // 파일 경로 생성
    std::string filePath = generateFilePath(event);
    
    // 녹화 시작
    if (startRecordingProcess(event, filePath)) {
        LOG_INFO("Started recording: {}", filePath);
        
        // 녹화 시간 대기
        std::this_thread::sleep_for(std::chrono::seconds(config_.recordDuration));
        
        // 녹화 종료
        {
            std::lock_guard<std::mutex> lock(recordingsMutex_);
            auto it = activeRecordings_.find(event.cameraIndex);
            if (it != activeRecordings_.end()) {
                kill(it->second, SIGTERM);
                
                int status;
                waitpid(it->second, &status, 0);
                activeRecordings_.erase(it);
                
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    LOG_INFO("Recording completed: {}", filePath);
                    notifyCompletion(event, filePath);
                } else {
                    LOG_ERROR("Recording failed with status: {}", status);
                }
            }
        }
    }
}

std::string EventRecorder::generateFilePath(const EventInfo& event) {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    std::stringstream ss;
    ss << config_.recordPath << "/";
    ss << "event_";
    
    // 이벤트 타입
    switch (event.type) {
        case EventType::HEAT: ss << "heat_"; break;
        case EventType::FLIP: ss << "flip_"; break;
        case EventType::LABOR_SIGN: ss << "labor_"; break;
        case EventType::OVER_TEMP: ss << "temp_"; break;
        case EventType::MANUAL: ss << "manual_"; break;
        default: ss << "unknown_"; break;
    }
    
    // 카메라 인덱스
    ss << "cam" << event.cameraIndex << "_";
    
    // 타임스탬프
    ss << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
    ss << "." << config_.recordingFormat;
    
    return ss.str();
}

bool EventRecorder::startRecordingProcess(const EventInfo& event, const std::string& filePath) {
    // FFmpeg 명령 구성
    std::stringstream cmd;
    
    // 입력: UDP 스트림 (파이프라인에서 출력되는 RTP 스트림)
    int basePort = 5000;
    int streamPort = basePort + (event.cameraIndex * 2);  // 메인 스트림 사용
    
    cmd << "ffmpeg -y ";  // 덮어쓰기
    cmd << "-i udp://127.0.0.1:" << streamPort << " ";  // 입력
    cmd << "-c copy ";  // 코덱 복사 (재인코딩 없음)
    cmd << "-t " << config_.recordDuration << " ";  // 녹화 시간
    cmd << "-f " << config_.recordingFormat << " ";  // 출력 포맷
    cmd << filePath << " ";
    cmd << "2>/dev/null";  // 에러 출력 숨김
    
    // 프로세스 실행
    pid_t pid = fork();
    if (pid == -1) {
        LOG_ERROR("Failed to fork recording process");
        return false;
    }
    
    if (pid == 0) {
        // 자식 프로세스
        execl("/bin/sh", "sh", "-c", cmd.str().c_str(), nullptr);
        _exit(1);  // exec 실패
    }
    
    // 부모 프로세스
    {
        std::lock_guard<std::mutex> lock(recordingsMutex_);
        activeRecordings_[event.cameraIndex] = pid;
    }
    
    return true;
}

void EventRecorder::notifyCompletion(const EventInfo& event, const std::string& filePath) {
    // 파일 크기 확인
    try {
        auto fileSize = std::filesystem::file_size(filePath);
        LOG_INFO("Recording file size: {} bytes", fileSize);
        
        if (fileSize < 1024) {  // 1KB 미만이면 오류로 간주
            LOG_ERROR("Recording file too small: {}", filePath);
            std::filesystem::remove(filePath);
            return;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to check recording file: {}", e.what());
        return;
    }
    
    // 콜백 호출
    if (completionCallback_) {
        completionCallback_(event, filePath);
    }
}