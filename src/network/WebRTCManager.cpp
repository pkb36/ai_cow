#include "network/WebRTCManager.hpp"
#include "core/Logger.hpp"

WebRTCManager::WebRTCManager(std::shared_ptr<Pipeline> pipeline)
    : pipeline_(pipeline), 
    asyncTasks_(std::make_unique<ThreadPool>(4)) {
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
    context->info.streamType = parseStreamType(source);
    context->info.connectedTime = std::chrono::steady_clock::now();
    context->info.state = WebRTCPeer::State::NEW;
    
    // WebRTC peer 생성
    WebRTCPeer::Config peerConfig;
    peerConfig.peerId = peerId;
    
    context->peer = std::make_unique<WebRTCPeer>(peerConfig);
    
    // 콜백 설정
    setupPeerCallbacks(context.get());
    
    // Pipeline에 스트림 추가
    if (!pipeline_->addStream(peerId, context->info.device, context->info.streamType)) {
        LOG_ERROR("Failed to add stream to pipeline");
        return false;
    }
    
    // Peer를 먼저 저장
    peers_[peerId] = std::move(context);
    
    // UDP source 생성 및 연결
    if (!createPeerConnection(peerId, source)) {
        LOG_ERROR("Failed to create peer connection for: {}", peerId);
        pipeline_->removeStream(peerId);
        peers_.erase(peerId);
        return false;
    }
    
    // *** 중요: Offer 생성 추가 ***
    auto it = peers_.find(peerId);
    if (it != peers_.end() && it->second->peer) {
        LOG_INFO("Creating offer for peer: {}", peerId);
        if (!it->second->peer->createOffer()) {
            LOG_ERROR("Failed to create offer for peer: {}", peerId);
        }
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
    
    // 동적 스트림 정보 가져오기
    auto dynamicInfo = pipeline_->getDynamicStreamInfo(peerId);
    if (dynamicInfo.has_value()) {
        context->streamPort = dynamicInfo->port;
        LOG_INFO("Using dynamic stream port {} for peer {}", 
                 context->streamPort, peerId);
    } else {
        LOG_ERROR("No dynamic stream info found for peer: {}", peerId);
        return false;
    }
    
    // UDP 소스 생성
    context->udpSrc = gst_element_factory_make("udpsrc", nullptr);
    if (!context->udpSrc) {
        LOG_ERROR("Failed to create UDP source");
        return false;
    }
    
    // RTP 캡슐화 확인
    std::string caps_str = "application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96";
    
    GstCaps* caps = gst_caps_from_string(caps_str.c_str());
    
    // UDP 소스 설정
    g_object_set(context->udpSrc, 
                 "port", context->streamPort,
                 "caps", caps,
                 "buffer-size", 2097152,  // 2MB
                 nullptr);
    
    gst_caps_unref(caps);
    
    LOG_INFO("Created UDP source on port {} for peer {}", 
             context->streamPort, peerId);
    
    // WebRTC peer에 연결
    if (!context->peer->connectToStream(context->udpSrc)) {
        LOG_ERROR("Failed to connect WebRTC peer to stream");
        gst_object_unref(context->udpSrc);
        context->udpSrc = nullptr;
        return false;
    }
    
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
    LOG_DEBUG("ICE candidate for peer {}: {}", peerId, candidate);
    
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
    
    // MessageHandler가 JSON 형식을 처리하므로 sdp를 그대로 전달
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

CameraDevice WebRTCManager::parseSource(const std::string& source) const {
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

StreamType WebRTCManager::parseStreamType(const std::string& source) const {
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

bool WebRTCManager::addPeerAsync(const std::string& peerId, const std::string& source) {
    // 중복 연결 요청 방지
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        auto it = pendingConnections_.find(peerId);
        if (it != pendingConnections_.end() && it->second.inProgress) {
            LOG_WARNING("Peer {} connection already in progress", peerId);
            return false;
        }
        
        PendingConnection pending;
        pending.peerId = peerId;
        pending.source = source;
        pending.startTime = std::chrono::steady_clock::now();
        pending.inProgress = true;
        
        pendingConnections_[peerId] = pending;
    }
    
    // 완전히 별도 스레드에서 처리
    std::thread([this, peerId, source]() {
        LOG_INFO("Starting async peer connection in detached thread: {}", peerId);
        
        try {
            // GStreamer 작업을 위한 임시 컨텍스트
            GMainContext* peerContext = g_main_context_new();
            g_main_context_push_thread_default(peerContext);
            
            bool success = addPeerSync(peerId, source);
            
            g_main_context_pop_thread_default(peerContext);
            g_main_context_unref(peerContext);
            
            // 결과 처리
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                auto it = pendingConnections_.find(peerId);
                if (it != pendingConnections_.end()) {
                    it->second.inProgress = false;
                    if (success) {
                        LOG_INFO("✅ Peer connection completed: {}", peerId);
                        pendingConnections_.erase(it);
                    } else {
                        LOG_ERROR("❌ Peer connection failed: {}", peerId);
                        pendingConnections_.erase(it);
                    }
                }
            }
            
        } catch (const std::exception& e) {
            LOG_ERROR("Exception in peer connection thread {}: {}", peerId, e.what());
        }
    }).detach();  // 스레드 분리
    
    return true;
}

bool WebRTCManager::addPeerSync(const std::string& peerId, const std::string& source) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    LOG_INFO("Adding peer synchronously: {} with source: {}", peerId, source);
    
    // 기존 addPeer 로직과 동일하지만 에러 처리 강화
    if (peers_.find(peerId) != peers_.end()) {
        LOG_WARNING("Peer already exists: {}", peerId);
        return false;
    }
    
    // Peer context 생성
    auto context = std::make_unique<PeerContext>();
    context->info.peerId = peerId;
    context->info.device = parseSource(source);
    context->info.streamType = parseStreamType(source);
    context->info.connectedTime = std::chrono::steady_clock::now();
    context->info.state = WebRTCPeer::State::NEW;
    
    // WebRTC peer 생성
    WebRTCPeer::Config peerConfig;
    peerConfig.peerId = peerId;
    
    context->peer = std::make_unique<WebRTCPeer>(peerConfig);
    
    // 콜백 설정
    setupPeerCallbacks(context.get());
    
    // Pipeline에 스트림 추가 (논블로킹으로 시도)
    LOG_DEBUG("Adding stream to pipeline for peer: {}", peerId);
    
    if (!pipeline_->addStream(peerId, context->info.device, context->info.streamType)) {
        LOG_ERROR("Failed to add stream to pipeline for peer: {}", peerId);
        return false;
    }
    
    LOG_DEBUG("Pipeline stream added successfully for peer: {}", peerId);
    
    // Peer를 먼저 저장
    peers_[peerId] = std::move(context);
    
    // UDP source 생성은 기존 방식 사용 (createPeerConnection)
    if (!createPeerConnection(peerId, source)) {
        LOG_ERROR("Failed to create peer connection for: {}", peerId);
        pipeline_->removeStream(peerId);
        peers_.erase(peerId);
        return false;
    }
    
    LOG_INFO("Peer added successfully: {}", peerId);
    return true;
}

bool WebRTCManager::createPeerConnectionAsync(const std::string& peerId, 
                                            const std::string& source) {
    auto it = peers_.find(peerId);
    if (it == peers_.end()) {
        LOG_ERROR("Peer not found: {}", peerId);
        return false;
    }
    
    auto& context = it->second;
    
    // 동적 스트림 정보 가져오기
    auto dynamicInfo = pipeline_->getDynamicStreamInfo(peerId);
    if (!dynamicInfo.has_value()) {
        LOG_ERROR("No dynamic stream info found for peer: {}", peerId);
        return false;
    }
    
    context->streamPort = dynamicInfo->port;
    LOG_INFO("Using dynamic stream port {} for peer {}", 
             context->streamPort, peerId);
    
    // UDP 소스 생성 (GStreamer 메인 스레드에서 실행)
    GMainContext* mainContext = g_main_context_default();
    
    bool udpCreated = false;
    GstElement* udpSrc = nullptr;
    
    // 매개변수를 스택에 생성
    struct UdpCreateParams {
        WebRTCManager* manager;
        std::string peerId;
        int port;
        bool* success;
        GstElement** element;
    };
    
    UdpCreateParams params = {
        this,
        peerId,
        context->streamPort,
        &udpCreated,
        &udpSrc
    };
    
    // 메인 스레드에서 UDP 소스 생성
    g_main_context_invoke_full(mainContext, G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            auto* params = static_cast<UdpCreateParams*>(data);
            
            *(params->element) = gst_element_factory_make("udpsrc", nullptr);
            if (*(params->element)) {
                std::string caps_str = "application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96";
                GstCaps* caps = gst_caps_from_string(caps_str.c_str());
                
                g_object_set(*(params->element), 
                             "port", params->port,
                             "caps", caps,
                             "buffer-size", 2097152,
                             nullptr);
                
                gst_caps_unref(caps);
                *(params->success) = true;
                
                LOG_INFO("Created UDP source on port {} for peer {}", params->port, params->peerId);
            } else {
                LOG_ERROR("Failed to create UDP source for peer {}", params->peerId);
                *(params->success) = false;
            }
            
            return G_SOURCE_REMOVE;
        },
        &params,
        nullptr
    );
    
    if (!udpCreated || !udpSrc) {
        LOG_ERROR("Failed to create UDP source");
        return false;
    }
    
    context->udpSrc = udpSrc;
    
    // WebRTC peer에 연결 (비동기)
    if (!context->peer->connectToStream(context->udpSrc)) {
        LOG_ERROR("Failed to connect WebRTC peer to stream");
        gst_object_unref(context->udpSrc);
        context->udpSrc = nullptr;
        return false;
    }
    
    return true;
}