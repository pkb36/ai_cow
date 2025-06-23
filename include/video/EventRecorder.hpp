#pragma once

#include <memory>
#include <string>
#include <chrono>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <unordered_map>
#include <vector>

enum class EventType {
    HEAT = 1,
    FLIP = 2,
    LABOR_SIGN = 3,
    OVER_TEMP = 4,
    MANUAL = 99
};

class EventRecorder {
public:
    struct EventInfo {
        EventType type;
        int cameraIndex;
        std::chrono::steady_clock::time_point timestamp;
        std::string description;
        std::string videoUrl;
    };

    struct Config {
        std::string recordPath = "/home/nvidia/data";
        int recordDuration = 60;        // seconds
        int preEventBuffer = 10;        // seconds
        int maxConcurrentRecordings = 2;
        std::string recordingFormat = "mp4";
    };

    static EventRecorder& getInstance() {
        static EventRecorder instance;
        return instance;
    }

    bool initialize(const Config& config);
    void shutdown();

    // 이벤트 트리거
    bool triggerEvent(EventType type, int cameraIndex, const std::string& description = "");
    
    // 수동 녹화 제어
    bool startManualRecording(int cameraIndex);
    bool stopManualRecording(int cameraIndex);
    
    // 상태 조회
    bool isRecording(int cameraIndex) const;
    size_t getActiveRecordingCount() const;
    std::vector<EventInfo> getRecentEvents(size_t count = 10) const;

    // 콜백
    using CompletionCallback = std::function<void(const EventInfo&, const std::string& filePath)>;
    void setCompletionCallback(CompletionCallback cb) { completionCallback_ = cb; }

private:
    EventRecorder() = default;
    ~EventRecorder();

    void recordingThread();
    void processEvent(const EventInfo& event);
    std::string generateFilePath(const EventInfo& event);
    bool startRecordingProcess(const EventInfo& event, const std::string& filePath);
    void notifyCompletion(const EventInfo& event, const std::string& filePath);

    Config config_;
    std::atomic<bool> running_{false};
    std::thread recordingThread_;
    
    std::queue<EventInfo> eventQueue_;
    mutable std::mutex queueMutex_;
    std::condition_variable queueCv_;
    
    std::unordered_map<int, pid_t> activeRecordings_;
    mutable std::mutex recordingsMutex_;
    
    std::vector<EventInfo> recentEvents_;
    mutable std::mutex eventsMutex_;
    
    CompletionCallback completionCallback_;
};