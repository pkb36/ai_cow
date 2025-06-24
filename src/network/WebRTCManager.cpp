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
    context->info.streamType = parseStreamType(source);  // 스트림 타입도 파싱
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
    
    // ✅ Peer를 먼저 저장 (createPeerConnection 호출 전에!)
    peers_[peerId] = std::move(context);
    
    // UDP source 생성 및 연결
    if (!createPeerConnection(peerId, source)) {
        LOG_ERROR("Failed to create peer connection for: {}", peerId);
        // 실패 시 정리
        pipeline_->removeStream(peerId);
        peers_.erase(peerId);  // 추가했던 peer 제거
        return false;
    }
    
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
    
    // 동적 스트림 제거
    pipeline_->removeStream(peerId);
    
    // UDP source 정리
    if (it->second->udpSrc) {
        gst_element_set_state(it->second->udpSrc, GST_STATE_NULL);
        gst_object_unref(it->second->udpSrc);
    }
    
    // Peer 제거
    peers_.erase(it);
    
    LOG_INFO("Peer removed: {} (remaining peers: {})", peerId, peers_.size());
    return true;
}

void WebRTCManager::removeAllPeers() {
    LOG_INFO("Removing all peers");
    
    // 복사본을 만들어서 순회 (iterator 무효화 방지)
    std::vector<std::string> peerIds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [peerId, context] : peers_) {
            peerIds.push_back(peerId);
        }
    }
    
    // 각 peer 제거
    for (const auto& peerId : peerIds) {
        removePeer(peerId);
    }
}

bool WebRTCManager::createPeerConnection(const std::string& peerId, 
                                        const std::string& source) {
    auto it = peers_.find(peerId);
    if (it == peers_.end()) {
        LOG_ERROR("Peer not found: {}", peerId);
        return false;
    }
    
    auto& context = it->second;
    
    // 스트림 정보 가져오기 - peerId로 먼저 시도
    auto streamInfo = pipeline_->getStreamInfo(peerId);
    
    // peerId로 찾을 수 없으면 device와 type으로 찾기
    if (!streamInfo.has_value()) {
        streamInfo = pipeline_->getStreamInfo(context->info.device, context->info.streamType);
    }
    
    if (!streamInfo.has_value() || streamInfo->port <= 0) {
        LOG_ERROR("Failed to get valid stream info for peer: {}", peerId);
        return false;
    }
    
    context->streamPort = streamInfo->port;
    
    // UDP 소스 생성
    context->udpSrc = gst_element_factory_make("udpsrc", nullptr);
    if (!context->udpSrc) {
        LOG_ERROR("Failed to create UDP source");
        return false;
    }
    
    // RTP 캡슐화 확인
    std::string caps_str = "application/x-rtp,media=video,encoding-name=H264,payload=96";
    if (source.find("VP8") != std::string::npos) {
        caps_str = "application/x-rtp,media=video,encoding-name=VP8,payload=96";
    } else if (source.find("VP9") != std::string::npos) {
        caps_str = "application/x-rtp,media=video,encoding-name=VP9,payload=96";
    }
    
    GstCaps* caps = gst_caps_from_string(caps_str.c_str());
    
    // UDP 소스 설정
    g_object_set(context->udpSrc, 
                 "port", context->streamPort,
                 "caps", caps,
                 "buffer-size", 2097152,  // 2MB
                 nullptr);
    
    gst_caps_unref(caps);
    
    LOG_DEBUG("Created UDP source on port {} with caps: {}", 
              context->streamPort, caps_str);
    
    // WebRTC peer에 연결
    if (!context->peer->connectToStream(context->udpSrc)) {
        LOG_ERROR("Failed to connect WebRTC peer to stream");
        gst_object_unref(context->udpSrc);
        context->udpSrc = nullptr;
        return false;
    }
    
    LOG_INFO("Created peer connection for {} on port {}", peerId, context->streamPort);
    return true;
}

bool WebRTCManager::handleOffer(const std::string& peerId, const std::string& sdp) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = peers_.find(peerId);
    if (it == peers_.end()) {
        LOG_ERROR("Peer not found: {}", peerId);
        return false;
    }
    
    LOG_DEBUG("Handling offer from peer: {}", peerId);
    
    return it->second->peer->setRemoteDescription("offer", sdp);
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
        nlohmann::json data;
        data["candidate"] = candidate;
        data["mlineIndex"] = mlineIndex;
        
        messageCallback_(peerId, "candidate", data.dump());
    }
}

void WebRTCManager::onOfferCreated(const std::string& peerId, const std::string& sdp) {
    LOG_DEBUG("Offer created for peer: {}", peerId);
    
    if (messageCallback_) {
        messageCallback_(peerId, "offer", sdp);
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
    
    // 연결 성공 시 통계 로깅
    if (newState == WebRTCPeer::State::CONNECTED) {
        LOG_INFO("WebRTC connection established for peer: {}", peerId);
        logConnectionStats();
    }
}

void WebRTCManager::onError(const std::string& peerId, const std::string& error) {
    LOG_ERROR("WebRTC error for peer {}: {}", peerId, error);
    
    // 에러 발생 시 해당 peer 제거
    removePeer(peerId);
}

CameraDevice WebRTCManager::parseSource(const std::string& source) {
    // 소스 문자열에서 디바이스 타입 파싱
    if (source.find("RGB") != std::string::npos || 
        source.find("rgb") != std::string::npos ||
        source == "0") {  // video0
        return CameraDevice::RGB;
    } else if (source.find("Thermal") != std::string::npos || 
               source.find("thermal") != std::string::npos ||
               source == "1") {  // video1
        return CameraDevice::THERMAL;
    }
    
    // 기본값
    return CameraDevice::RGB;
}

StreamType WebRTCManager::parseStreamType(const std::string& source) {
    // 소스 문자열에서 스트림 타입 파싱
    if (source.find("sub") != std::string::npos || 
        source.find("secondary") != std::string::npos ||
        source.find("enc2") != std::string::npos) {
        return StreamType::SECONDARY;
    }
    
    // 기본값은 MAIN
    return StreamType::MAIN;
}

void WebRTCManager::logConnectionStats() const {
    size_t totalPeers = peers_.size();
    size_t connectedPeers = 0;
    
    std::unordered_map<CameraDevice, size_t> deviceCount;
    std::unordered_map<StreamType, size_t> streamTypeCount;
    
    for (const auto& [peerId, context] : peers_) {
        if (context->peer->isConnected()) {
            connectedPeers++;
        }
        deviceCount[context->info.device]++;
        streamTypeCount[context->info.streamType]++;
    }
    
    LOG_INFO("=== WebRTC Connection Statistics ===");
    LOG_INFO("Total peers: {}", totalPeers);
    LOG_INFO("Connected peers: {}", connectedPeers);
    LOG_INFO("RGB streams: {}", deviceCount[CameraDevice::RGB]);
    LOG_INFO("Thermal streams: {}", deviceCount[CameraDevice::THERMAL]);
    LOG_INFO("Main streams: {}", streamTypeCount[StreamType::MAIN]);
    LOG_INFO("Secondary streams: {}", streamTypeCount[StreamType::SECONDARY]);
    LOG_INFO("==================================");
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