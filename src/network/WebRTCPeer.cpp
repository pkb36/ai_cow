#include "network/WebRTCPeer.hpp"
#include "core/Logger.hpp"
#include <nlohmann/json.hpp>

struct WebRTCPeer::Impl {
    GstElement* webrtcbin = nullptr;
    GstElement* pipeline = nullptr;
    GstPromise* promise = nullptr;
    
    // 시그널 핸들러 ID들
    gulong onNegotiationNeededId = 0;
    gulong onIceCandidateId = 0;
    gulong onIceGatheringStateId = 0;
    gulong onConnectionStateId = 0;
    
    ~Impl() {
        cleanup();
    }
    
    void cleanup() {
        if (webrtcbin) {
            if (onNegotiationNeededId) {
                g_signal_handler_disconnect(webrtcbin, onNegotiationNeededId);
            }
            if (onIceCandidateId) {
                g_signal_handler_disconnect(webrtcbin, onIceCandidateId);
            }
            if (onIceGatheringStateId) {
                g_signal_handler_disconnect(webrtcbin, onIceGatheringStateId);
            }
            if (onConnectionStateId) {
                g_signal_handler_disconnect(webrtcbin, onConnectionStateId);
            }
        }
        
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
        
        webrtcbin = nullptr;  // pipeline이 webrtcbin을 소유하므로 별도 unref 불필요
    }
    
    // 정적 콜백 함수들
    static void onNegotiationNeeded(GstElement* element, gpointer userData);
    static void onIceCandidate(GstElement* element, guint mlineIndex, gchar* candidate, gpointer userData);
    static void onIceGatheringState(GstElement* element, GParamSpec* pspec, gpointer userData);
    static void onConnectionState(GstElement* element, GParamSpec* pspec, gpointer userData);
    static void onOfferCreated(GstPromise* promise, gpointer userData);
    static void onAnswerCreated(GstPromise* promise, gpointer userData);
};

WebRTCPeer::WebRTCPeer(const Config& config) 
    : impl_(std::make_unique<Impl>()), config_(config) {
    LOG_TRACE("Creating WebRTC peer for: {}", config.peerId);
}

WebRTCPeer::~WebRTCPeer() {
    disconnect();
}

bool WebRTCPeer::connectToStream(GstElement* udpSrc) {
    if (!udpSrc) {
        LOG_ERROR("Invalid UDP source");
        return false;
    }
    
    // 파이프라인 생성
    impl_->pipeline = gst_pipeline_new(nullptr);
    if (!impl_->pipeline) {
        LOG_ERROR("Failed to create pipeline");
        return false;
    }
    
    // WebRTC bin 생성
    impl_->webrtcbin = gst_element_factory_make("webrtcbin", "webrtc");
    if (!impl_->webrtcbin) {
        LOG_ERROR("Failed to create webrtcbin element");
        gst_object_unref(impl_->pipeline);
        impl_->pipeline = nullptr;
        return false;
    }
    
    // 설정 - bundle-policy 추가
    g_object_set(impl_->webrtcbin,
                 "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,
                 "stun-server", config_.stunServer.c_str(),
                 "latency", 0,  // 레이턴시 0으로 설정
                 nullptr);
    
    // RTP jitter buffer 추가
    GstElement* rtpjitterbuffer = gst_element_factory_make("rtpjitterbuffer", nullptr);
    if (!rtpjitterbuffer) {
        LOG_ERROR("Failed to create rtpjitterbuffer");
        gst_object_unref(impl_->webrtcbin);
        gst_object_unref(impl_->pipeline);
        impl_->pipeline = nullptr;
        impl_->webrtcbin = nullptr;
        return false;
    }
    
    // jitterbuffer 설정
    g_object_set(rtpjitterbuffer,
                 "latency", 200,  // 200ms
                 "mode", 0,       // RTP_JITTER_BUFFER_MODE_NONE
                 "do-lost", TRUE,
                 nullptr);
    
    // 파이프라인에 추가
    gst_bin_add_many(GST_BIN(impl_->pipeline), udpSrc, rtpjitterbuffer, impl_->webrtcbin, nullptr);
    
    // 링크: udpsrc -> rtpjitterbuffer
    if (!gst_element_link(udpSrc, rtpjitterbuffer)) {
        LOG_ERROR("Failed to link udpsrc to rtpjitterbuffer");
        gst_object_unref(impl_->pipeline);
        impl_->pipeline = nullptr;
        impl_->webrtcbin = nullptr;
        return false;
    }
    
    // rtpjitterbuffer -> webrtcbin 연결
    GstPad* srcPad = gst_element_get_static_pad(rtpjitterbuffer, "src");
    GstPad* sinkPad = gst_element_get_request_pad(impl_->webrtcbin, "sink_%u");
    
    if (!sinkPad) {
        LOG_ERROR("Failed to get sink pad from webrtcbin");
        gst_object_unref(srcPad);
        gst_object_unref(impl_->pipeline);
        impl_->pipeline = nullptr;
        impl_->webrtcbin = nullptr;
        return false;
    }
    
    if (gst_pad_link(srcPad, sinkPad) != GST_PAD_LINK_OK) {
        LOG_ERROR("Failed to link to webrtcbin");
        gst_object_unref(srcPad);
        gst_object_unref(sinkPad);
        gst_object_unref(impl_->pipeline);
        impl_->pipeline = nullptr;
        impl_->webrtcbin = nullptr;
        return false;
    }
    
    gst_object_unref(srcPad);
    gst_object_unref(sinkPad);
    
    // 시그널 연결
    impl_->onNegotiationNeededId = g_signal_connect(impl_->webrtcbin, 
        "on-negotiation-needed", G_CALLBACK(Impl::onNegotiationNeeded), this);
    
    impl_->onIceCandidateId = g_signal_connect(impl_->webrtcbin, 
        "on-ice-candidate", G_CALLBACK(Impl::onIceCandidate), this);
    
    impl_->onIceGatheringStateId = g_signal_connect(impl_->webrtcbin, 
        "notify::ice-gathering-state", G_CALLBACK(Impl::onIceGatheringState), this);
    
    impl_->onConnectionStateId = g_signal_connect(impl_->webrtcbin, 
        "notify::connection-state", G_CALLBACK(Impl::onConnectionState), this);
    
    // 파이프라인 시작
    GstStateChangeReturn ret = gst_element_set_state(impl_->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Failed to start WebRTC pipeline");
        gst_object_unref(impl_->pipeline);
        impl_->pipeline = nullptr;
        impl_->webrtcbin = nullptr;
        return false;
    }
    
    setState(State::CONNECTING);
    LOG_INFO("WebRTC pipeline created for peer: {}", config_.peerId);
    return true;
}

bool WebRTCPeer::createOffer() {
    if (!impl_->webrtcbin) {
        LOG_ERROR("WebRTC not initialized");
        return false;
    }
    
    // Offer 생성
    impl_->promise = gst_promise_new_with_change_func(
        Impl::onOfferCreated, this, nullptr);
    
    g_signal_emit_by_name(impl_->webrtcbin, "create-offer", nullptr, impl_->promise);
    
    return true;
}

bool WebRTCPeer::setRemoteDescription(const std::string& type, const std::string& sdp) {
    if (!impl_->webrtcbin) {
        LOG_ERROR("WebRTC not initialized");
        return false;
    }
    
    // SDP 파싱
    GstSDPMessage* sdpMsg;
    gst_sdp_message_new(&sdpMsg);
    
    if (gst_sdp_message_parse_buffer((const guint8*)sdp.c_str(), sdp.length(), sdpMsg) != GST_SDP_OK) {
        LOG_ERROR("Failed to parse SDP");
        gst_sdp_message_free(sdpMsg);
        return false;
    }
    
    // WebRTC 세션 설명 생성
    GstWebRTCSessionDescription* description = gst_webrtc_session_description_new(
        type == "offer" ? GST_WEBRTC_SDP_TYPE_OFFER : GST_WEBRTC_SDP_TYPE_ANSWER,
        sdpMsg
    );
    
    // Remote description 설정
    GstPromise* promise = gst_promise_new();
    g_signal_emit_by_name(impl_->webrtcbin, "set-remote-description", description, promise);
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);
    
    gst_webrtc_session_description_free(description);
    
    // Answer인 경우 연결 완료
    if (type == "answer") {
        setState(State::CONNECTED);
    }
    // Offer인 경우 Answer 생성
    else if (type == "offer") {
        impl_->promise = gst_promise_new_with_change_func(
            Impl::onAnswerCreated, this, nullptr);
        g_signal_emit_by_name(impl_->webrtcbin, "create-answer", nullptr, impl_->promise);
    }
    
    return true;
}

bool WebRTCPeer::addIceCandidate(const std::string& candidate, int mlineIndex) {
    if (!impl_->webrtcbin) {
        LOG_ERROR("WebRTC not initialized");
        return false;
    }
    
    // ICE candidate 추가
    g_signal_emit_by_name(impl_->webrtcbin, "add-ice-candidate", 
                         (guint)mlineIndex, candidate.c_str());
    
    return true;
}

void WebRTCPeer::disconnect() {
    if (state_ == State::CLOSED) {
        return;
    }
    
    LOG_INFO("Disconnecting peer: {}", config_.peerId);
    
    setState(State::CLOSED);
    impl_->cleanup();
}

void WebRTCPeer::setState(State newState) {
    if (state_ == newState) {
        return;
    }
    
    State oldState = state_;
    state_ = newState;
    
    LOG_DEBUG("Peer {} state changed: {} -> {}", 
              config_.peerId, 
              static_cast<int>(oldState), 
              static_cast<int>(newState));
    
    if (stateChangeCallback_) {
        stateChangeCallback_(oldState, newState);
    }
}

// 콜백 구현들
void WebRTCPeer::Impl::onNegotiationNeeded(GstElement* element, gpointer userData) {
    (void)element; // 미사용 매개변수 경고 제거
    auto* peer = static_cast<WebRTCPeer*>(userData);
    LOG_DEBUG("Negotiation needed for peer: {}", peer->config_.peerId);
    
    peer->createOffer();
}

void WebRTCPeer::Impl::onIceCandidate(GstElement* element, guint mlineIndex, 
                                      gchar* candidate, gpointer userData) {
    (void)element; // 미사용 매개변수 경고 제거
    auto* peer = static_cast<WebRTCPeer*>(userData);
    
    if (peer->iceCandidateCallback_) {
        peer->iceCandidateCallback_(std::string(candidate), static_cast<int>(mlineIndex));
    }
}

void WebRTCPeer::Impl::onIceGatheringState(GstElement* element, GParamSpec* pspec, gpointer userData) {
    (void)element; (void)pspec; (void)userData; // 미사용 매개변수 경고 제거
    GstWebRTCICEGatheringState ice_gather_state;
    const gchar *new_state = "unknown";
    
    g_object_get (element, "ice-gathering-state", &ice_gather_state, NULL);
    switch (ice_gather_state) {
        case GST_WEBRTC_ICE_GATHERING_STATE_NEW:
        new_state = "new";
        break;
        case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING:
        new_state = "gathering";
        break;
        case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE:
        new_state = "complete";
        // ICE complete 후 DTLS 준비 시간
        g_usleep(100000);  // 100ms 대기
        break;
    }
    LOG_DEBUG("ICE gathering state changed: {}", new_state);
}

void WebRTCPeer::Impl::onConnectionState(GstElement* element, GParamSpec* pspec, gpointer userData) {
    (void)element; (void)pspec; (void)userData; // 미사용 매개변수 경고 제거
    // 연결 상태 변경 처리 (필요시 구현)
}

void WebRTCPeer::Impl::onOfferCreated(GstPromise* promise, gpointer userData) {
    auto* peer = static_cast<WebRTCPeer*>(userData);
    
    const GstStructure* reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription* offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
    
    if (!offer) {
        LOG_ERROR("Failed to create offer");
        if (peer->errorCallback_) {
            peer->errorCallback_("Failed to create offer");
        }
        return;
    }
    
    // Local description 설정
    GstPromise* setPromise = gst_promise_new();
    g_signal_emit_by_name(peer->impl_->webrtcbin, "set-local-description", offer, setPromise);
    gst_promise_interrupt(setPromise);
    gst_promise_unref(setPromise);
    
    // SDP 문자열로 변환
    gchar* sdpStr = gst_sdp_message_as_text(offer->sdp);
    
    if (peer->offerCreatedCallback_) {
        peer->offerCreatedCallback_(std::string(sdpStr));
    }
    
    g_free(sdpStr);
    gst_webrtc_session_description_free(offer);
}

void WebRTCPeer::Impl::onAnswerCreated(GstPromise* promise, gpointer userData) {
    auto* peer = static_cast<WebRTCPeer*>(userData);
    
    const GstStructure* reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription* answer = nullptr;
    gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, nullptr);
    
    if (!answer) {
        LOG_ERROR("Failed to create answer");
        if (peer->errorCallback_) {
            peer->errorCallback_("Failed to create answer");
        }
        return;
    }
    
    // Local description 설정
    GstPromise* setPromise = gst_promise_new();
    g_signal_emit_by_name(peer->impl_->webrtcbin, "set-local-description", answer, setPromise);
    gst_promise_interrupt(setPromise);
    gst_promise_unref(setPromise);
    
    // SDP 문자열로 변환
    gchar* sdpStr = gst_sdp_message_as_text(answer->sdp);
    
    // Answer는 자동으로 전송하지 않고 콜백만 호출
    LOG_DEBUG("Answer created for peer: {}", peer->config_.peerId);
    
    g_free(sdpStr);
    gst_webrtc_session_description_free(answer);
}

// 통계 정보 가져오기
WebRTCPeer::Statistics WebRTCPeer::getStatistics() const {
    Statistics stats;
    
    if (!impl_->webrtcbin) {
        return stats;
    }
    
    // GstPromise를 사용하여 통계 가져오기
    GstPromise* promise = gst_promise_new();
    g_signal_emit_by_name(impl_->webrtcbin, "get-stats", nullptr, promise);
    
    GstPromiseResult result = gst_promise_wait(promise);
    if (result == GST_PROMISE_RESULT_REPLIED) {
        const GstStructure* reply = gst_promise_get_reply(promise);
        if (reply) {
            // 통계 파싱 (실제 구현은 WebRTC 통계 구조에 따라 다름)
            // 여기서는 예시로 간단히 구현
            LOG_TRACE("Stats structure: {}", gst_structure_to_string(reply));
        }
    }
    
    gst_promise_unref(promise);
    return stats;
}