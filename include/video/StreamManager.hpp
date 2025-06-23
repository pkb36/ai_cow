#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <set>  // std::set을 위해 추가
#include "video/Pipeline.hpp"

// 동적 스트림 관리
class StreamManager {
public:
    struct StreamConfig {
        std::string peerId;
        CameraDevice device;
        StreamType type;
        int port;
        bool active;
    };

    StreamManager(std::shared_ptr<Pipeline> pipeline);
    ~StreamManager();

    // 스트림 생명주기 관리
    bool createStream(const std::string& peerId, const std::string& source);
    bool removeStream(const std::string& peerId);
    bool removeAllStreams();

    // 스트림 정보 조회
    std::optional<StreamConfig> getStreamConfig(const std::string& peerId) const;
    std::vector<StreamConfig> getAllStreams() const;
    
    // 스트림 상태 관리
    bool isStreamActive(const std::string& peerId) const;
    size_t getActiveStreamCount() const;

private:
    std::shared_ptr<Pipeline> pipeline_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, StreamConfig> streams_;
    
    // 포트 할당 관리
    std::set<int> usedPorts_;
    int allocatePort();
    void releasePort(int port);
};