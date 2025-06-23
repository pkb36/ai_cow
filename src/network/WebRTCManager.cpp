#include "network/WebRTCManager.hpp"
#include "core/Logger.hpp"

WebRTCManager::WebRTCManager(std::shared_ptr<Pipeline> pipeline)
    : pipeline_(pipeline) {
    LOG_TRACE("WebRTCManager created");
}

WebRTCManager::~WebRTCManager() {
    removeAllPeers();
}

bool WebRTCManager::addPeer(const std::string& peerId, const std::string& source) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 이미 존재하는지 확인
    if (peers_.find(peerId) != peers_.end()) {
        LOG_WARNING("Peer already exists: {}", peerId);
        return false;
    }
    
    LOG_INFO("Adding peer: {} with source: {}", peerId, source);
    
    // Peer context 생성
    auto context = std::make_unique<PeerContext>();
    context->info.peerId = peerId;
    context->info.device = parseSource(source);
    context->info.streamType = StreamType::MAIN;  // 기본값
    context->info.connectedTime = std::chrono::steady_clock::now();
    context->info.state = WebRTCPeer::State::NEW;
    
    // WebRTC peer 생성
    WebRTCPeer::Config peerConfig;
    peerConfig.peerId = peerId;
    // STUN/TURN 서버 설정은 전역 설정에서 가져옴
    
    context->peer = std::make_unique<WebRTCPeer>(peerConfig);
    
    // 콜백 설정
    setupPeerCallbacks(context.get());
    
    // Pipeline에 스트림 추가
    if (!pipeline_->addStream(peerId, context->info.device, context->info.streamType)) {
        LOG_ERROR("Failed to add stream to pipeline");
        return false;
    }
    
    // UDP source 생성 및 연결
    if (!createPeerConnection(peerId, source)) {
        pipeline_->removeStream(peerId);
        return false;
    }
    
    // Peer 저장
    peers_[peerId] = std::move(context);
    
    LOG_INFO("Peer added successfully: {}", peerId);
    return true;
}

bool WebRTCManager::removePeer(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = peers_.find(peerId);
    if (it == peers_.end()) {
        LOG_WARNING("Peer not found: {}", peerId);
        return false;
    }
    
    LOG_INFO("Removing peer: {}", peerId);
    
    // WebRTC 연결 종료
    it->second->peer->disconnect();
    
    // Pipeline에서 스트림 제거
    pipeline_->removeStream(peerId);
    
    // UDP source 정리
    if (it->second->udpSrc) {
        gst_element_set_state(it->second->udpSrc, GST_STATE_NULL);
        gst_object_unref(it->second->udpSrc);
    }
    
    // Peer 제거
    peers_.erase(it);
    
    LOG_INFO("Peer removed: {}", peerId);
    return true;
}

void WebRTCManager::removeAllPeers() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    LOG_INFO("Removing all {} peers", peers_.size());
    
    for (auto& [peerId, context] : peers_) {
        if (context->peer) {
            context->peer->disconnect();
        }
        pipeline_->removeStream(peerId);
        
        if (context->udpSrc) {
            gst_element_set_state(context->udpSrc, GST_STATE_NULL);
            gst_object_unref(context->udpSrc);
        }
    }
    
    peers_.clear();
}

bool WebRTCManager::createPeerConnection(const std::string& peerId, const std::string& source) {
    // UDP 소스 생성 (파이프라인의 UDP 싱크에서 스트림 받기)
    auto it = peers_.find(peerId);
    if (it == peers_.end()) {
        return false;
    }
    
    auto& context = it->second;
    
    // 스트림 포트 계산 (Pipeline의 포트 할당 로직과 일치해야 함)
    int basePort = 5000;
    int deviceIndex = static_cast<int>(context->info.device);
    int streamTypeIndex = static_cast<int>(context->info.streamType);
    
    // 간단한 포트 할당 (실제로는 Pipeline에서 동적으로 할당받아야 함)
    context->streamPort = basePort + deviceIndex * 2 + streamTypeIndex;
    
    // UDP 소스 생성
    context->udpSrc = gst_element_factory_make("udpsrc", nullptr);
    if (!context->udpSrc) {
        LOG_ERROR("Failed to create UDP source");
        return false;
    }
    
    // UDP 소스 설정
    g_object_set(context->udpSrc, 
                 "port", context->streamPort,
                 "caps", gst_caps_from_string("application/x-rtp"),
                 nullptr);
    
    // WebRTC peer에 스트림 연결
    if (!context->peer->connectToStream(context->udpSrc)) {
        LOG_ERROR("Failed to connect stream to WebRTC peer");
        gst_object_unref(context->udpSrc);
        context->udpSrc = nullptr;
        return false;
    }
    
    return true;
}

bool WebRTCManager::handleAnswer(const std::string& peerId, const std::string& sdp) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = peers_.find(peerId);
    if (it == peers_.end()) {
        LOG_ERROR("Peer not found: {}", peerId);
        return false;
    }
    
    LOG_DEBUG("Handling answer from peer: {}", peerId);
    
    return it->second->peer->setRemoteDescription("answer", sdp);
}

bool WebRTCManager::handleIceCandidate(const std::string& peerId, 
                                       const std::string& candidate, 
                                       int mlineIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = peers_.find(peerId);
    if (it == peers_.end()) {
        LOG_ERROR("Peer not found: {}", peerId);
        return false;
    }
    
    LOG_TRACE("Adding ICE candidate for peer: {}", peerId);
    
    return it->second->peer->addIceCandidate(candidate, mlineIndex);
}

void WebRTCManager::setupPeerCallbacks(PeerContext* context) {
    if (!context || !context->peer) return;
    
    const std::string peerId = context->info.peerId;
    
    // ICE candidate 콜백
    context->peer->setIceCandidateCallback(
        [this, peerId](const std::string& candidate, int mlineIndex) {
            onIceCandidate(peerId, candidate, mlineIndex);
        }
    );
    
    // Offer 생성 콜백
    context->peer->setOfferCreatedCallback(
        [this, peerId](const std::string& sdp) {
            onOfferCreated(peerId, sdp);
        }
    );
    
    // 상태 변경 콜백
    context->peer->setStateChangeCallback(
        [this, peerId](WebRTCPeer::State oldState, WebRTCPeer::State newState) {
            onStateChange(peerId, oldState, newState);
        }
    );
    
    // 에러 콜백
    context->peer->setErrorCallback(
        [this, peerId](const std::string& error) {
            onError(peerId, error);
        }
    );
}

void WebRTCManager::onIceCandidate(const std::string& peerId, 
                                   const std::string& candidate, 
                                   int mlineIndex) {
    LOG_TRACE("ICE candidate for peer {}: {}", peerId, candidate);
    
    if (messageCallback_) {
        // JSON 형식으로 메시지 생성
        nlohmann::json root;
        root["peerId"] = peerId;
        root["type"] = "candidate";
        root["candidate"] = candidate;
        root["mlineIndex"] = mlineIndex;
        
        std::string jsonStr = root.dump();
        messageCallback_(peerId, "candidate", jsonStr);
    }
}

void WebRTCManager::onOfferCreated(const std::string& peerId, const std::string& sdp) {
    LOG_DEBUG("Offer created for peer: {}", peerId);
    
    if (messageCallback_) {
        nlohmann::json root;
        root["peerId"] = peerId;
        root["type"] = "offer";
        root["sdp"] = sdp;
        
        std::string jsonStr = root.dump();
        messageCallback_(peerId, "offer", jsonStr);
    }
}

void WebRTCManager::onStateChange(const std::string& peerId, 
                                  WebRTCPeer::State oldState, 
                                  WebRTCPeer::State newState) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = peers_.find(peerId);
    if (it != peers_.end()) {
        it->second->info.state = newState;
    }
    
    LOG_INFO("Peer {} state changed: {} -> {}", 
             peerId, static_cast<int>(oldState), static_cast<int>(newState));
}

void WebRTCManager::onError(const std::string& peerId, const std::string& error) {
    LOG_ERROR("WebRTC error for peer {}: {}", peerId, error);
    
    // 에러 발생 시 해당 peer 제거
    removePeer(peerId);
}

CameraDevice WebRTCManager::parseSource(const std::string& source) const {
    if (source == "RGB" || source == "rgb") {
        return CameraDevice::RGB;
    } else if (source == "Thermal" || source == "thermal") {
        return CameraDevice::THERMAL;
    }
    
    // 기본값
    return CameraDevice::RGB;
}

std::optional<WebRTCManager::PeerInfo> WebRTCManager::getPeerInfo(const std::string& peerId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = peers_.find(peerId);
    if (it != peers_.end()) {
        return it->second->info;
    }
    
    return std::nullopt;
}

std::vector<WebRTCManager::PeerInfo> WebRTCManager::getAllPeers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<PeerInfo> result;
    result.reserve(peers_.size());
    
    for (const auto& [peerId, context] : peers_) {
        result.push_back(context->info);
    }
    
    return result;
}

size_t WebRTCManager::getPeerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peers_.size();
}

// 통계 정보 수집
WebRTCManager::GlobalStatistics WebRTCManager::getGlobalStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    GlobalStatistics stats;
    stats.totalPeers = peers_.size();
    
    for (const auto& [peerId, context] : peers_) {
        if (context->peer->isConnected()) {
            stats.activePeers++;
            
            auto peerStats = context->peer->getStatistics();
            stats.totalBytesSent += peerStats.bytesSent;
        }
    }
    
    if (stats.activePeers > 0) {
        stats.averageBitrate = static_cast<double>(stats.totalBytesSent * 8) / 
                              (stats.activePeers * 1000000.0); // Mbps
    }
    
    return stats;
}