#include "video/StreamManager.hpp"
#include "core/Logger.hpp"
#include <algorithm>

StreamManager::StreamManager(std::shared_ptr<Pipeline> pipeline)
    : pipeline_(pipeline) {
    LOG_TRACE("StreamManager created");
}

StreamManager::~StreamManager() {
    removeAllStreams();
}

bool StreamManager::createStream(const std::string& peerId, const std::string& source) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 이미 존재하는지 확인
    if (streams_.find(peerId) != streams_.end()) {
        LOG_WARNING("Stream already exists for peer: {}", peerId);
        return false;
    }
    
    // 소스 파싱
    CameraDevice device = CameraDevice::RGB;
    StreamType type = StreamType::MAIN;
    
    if (source.find("RGB") != std::string::npos || source.find("rgb") != std::string::npos) {
        device = CameraDevice::RGB;
    } else if (source.find("Thermal") != std::string::npos || source.find("thermal") != std::string::npos) {
        device = CameraDevice::THERMAL;
    }
    
    if (source.find("sub") != std::string::npos || source.find("secondary") != std::string::npos) {
        type = StreamType::SECONDARY;
    }
    
    // 포트 할당
    int port = allocatePort();
    if (port < 0) {
        LOG_ERROR("Failed to allocate port for stream");
        return false;
    }
    
    // 파이프라인에 스트림 추가
    if (!pipeline_->addStream(peerId, device, type)) {
        releasePort(port);
        LOG_ERROR("Failed to add stream to pipeline");
        return false;
    }
    
    // 스트림 정보 저장
    StreamConfig config;
    config.peerId = peerId;
    config.device = device;
    config.type = type;
    config.port = port;
    config.active = true;
    
    streams_[peerId] = config;
    
    LOG_INFO("Created stream for peer {} on port {} (device: {}, type: {})", 
             peerId, port, static_cast<int>(device), static_cast<int>(type));
    
    return true;
}

bool StreamManager::removeStream(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = streams_.find(peerId);
    if (it == streams_.end()) {
        LOG_WARNING("Stream not found for peer: {}", peerId);
        return false;
    }
    
    // 파이프라인에서 스트림 제거
    pipeline_->removeStream(peerId);
    
    // 포트 해제
    releasePort(it->second.port);
    
    // 스트림 정보 제거
    streams_.erase(it);
    
    LOG_INFO("Removed stream for peer: {}", peerId);
    return true;
}

bool StreamManager::removeAllStreams() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    LOG_INFO("Removing all {} streams", streams_.size());
    
    for (const auto& [peerId, config] : streams_) {
        pipeline_->removeStream(peerId);
        releasePort(config.port);
    }
    
    streams_.clear();
    usedPorts_.clear();
    
    return true;
}

std::optional<StreamManager::StreamConfig> 
StreamManager::getStreamConfig(const std::string& peerId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = streams_.find(peerId);
    if (it != streams_.end()) {
        return it->second;
    }
    
    return std::nullopt;
}

std::vector<StreamManager::StreamConfig> StreamManager::getAllStreams() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<StreamConfig> result;
    result.reserve(streams_.size());
    
    for (const auto& [peerId, config] : streams_) {
        result.push_back(config);
    }
    
    return result;
}

bool StreamManager::isStreamActive(const std::string& peerId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = streams_.find(peerId);
    return it != streams_.end() && it->second.active;
}

size_t StreamManager::getActiveStreamCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    return std::count_if(streams_.begin(), streams_.end(),
        [](const auto& pair) { return pair.second.active; });
}

int StreamManager::allocatePort() {
    // 기본 포트부터 시작
    const int basePort = 5000;
    const int maxPort = 6000;
    
    for (int port = basePort; port < maxPort; port += 2) {
        if (usedPorts_.find(port) == usedPorts_.end()) {
            usedPorts_.insert(port);
            return port;
        }
    }
    
    return -1;  // 사용 가능한 포트 없음
}

void StreamManager::releasePort(int port) {
    usedPorts_.erase(port);
}