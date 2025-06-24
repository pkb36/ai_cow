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
    static void busMessage(GstBus* bus, GstMessage* message, gpointer userData);
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
    
    // 방법 1: 파이프라인 문자열로 생성하는 대신 프로그래밍 방식으로 생성
    impl_->pipeline = gst_pipeline_new(nullptr);
    if (!impl_->pipeline) {
        LOG_ERROR("Failed to create pipeline");
        return false;
    }
    
    // WebRTCBin 엘리먼트 직접 생성
    impl_->webrtcbin = gst_element_factory_make("webrtcbin", "webrtc");
    if (!impl_->webrtcbin) {
        LOG_ERROR("Failed to create webrtcbin element - check if gstreamer1.0-plugins-bad is installed");
        gst_object_unref(impl_->pipeline);
        impl_->pipeline = nullptr;
        return false;
    }
    
    // WebRTCBin 설정
    g_object_set(impl_->webrtcbin,
                 "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,
                 "stun-server", config_.stunServer.c_str(),
                 nullptr);
    
    if (config_.useTurn && !config_.turnServer.empty()) {
        g_object_set(impl_->webrtcbin,
                     "turn-server", config_.turnServer.c_str(),
                     nullptr);
    }
    
    // RTP 엘리먼트들 생성
    GstElement* rtpDepay = gst_element_factory_make("rtph264depay", nullptr);
    GstElement* h264parse = gst_element_factory_make("h264parse", nullptr);
    GstElement* rtpPay = gst_element_factory_make("rtph264pay", nullptr);
    
    if (!rtpDepay || !h264parse || !rtpPay) {
        LOG_ERROR("Failed to create RTP elements");
        if (rtpDepay) gst_object_unref(rtpDepay);
        if (h264parse) gst_object_unref(h264parse);
        if (rtpPay) gst_object_unref(rtpPay);
        gst_object_unref(impl_->webrtcbin);
        gst_object_unref(impl_->pipeline);
        impl_->webrtcbin = nullptr;
        impl_->pipeline = nullptr;
        return false;
    }
    
    // RTP payloader 설정
    g_object_set(rtpPay,
                 "config-interval", 1,
                 "pt", 96,
                 nullptr);
    
    // 파이프라인에 엘리먼트 추가
    gst_bin_add_many(GST_BIN(impl_->pipeline), 
                     udpSrc, rtpDepay, h264parse, rtpPay, impl_->webrtcbin, 
                     nullptr);
    
    // 엘리먼트 링크
    if (!gst_element_link_many(udpSrc, rtpDepay, h264parse, rtpPay, nullptr)) {
        LOG_ERROR("Failed to link elements before webrtcbin");
        gst_object_unref(impl_->pipeline);
        impl_->pipeline = nullptr;
        impl_->webrtcbin = nullptr;
        return false;
    }
    
    // RTP payloader를 WebRTCBin에 연결
    GstPad* srcPad = gst_element_get_static_pad(rtpPay, "src");
    if (!srcPad) {
        LOG_ERROR("Failed to get src pad from rtppay");
        gst_object_unref(impl_->pipeline);
        impl_->pipeline = nullptr;
        impl_->webrtcbin = nullptr;
        return false;
    }
    
    // caps 설정
    GstCaps* caps = gst_caps_from_string(
        "application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000");
    g_object_set(rtpPay, "caps", caps, nullptr);
    gst_caps_unref(caps);
    
    // WebRTCBin sink pad 요청
    GstPad* sinkPad = gst_element_get_request_pad(impl_->webrtcbin, "sink_%u");
    if (!sinkPad) {
        LOG_ERROR("Failed to get sink pad from webrtcbin");
        gst_object_unref(srcPad);
        gst_object_unref(impl_->pipeline);
        impl_->pipeline = nullptr;
        impl_->webrtcbin = nullptr;
        return false;
    }
    
    // 패드 연결
    GstPadLinkReturn linkRet = gst_pad_link(srcPad, sinkPad);
    if (linkRet != GST_PAD_LINK_OK) {
        LOG_ERROR("Failed to link pads: {}", gst_pad_link_get_name(linkRet));
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
    
    // 버스 메시지 핸들러 추가
    GstBus* bus = gst_element_get_bus(impl_->pipeline);
    gst_bus_add_signal_watch(bus);
    g_signal_connect(bus, "message", G_CALLBACK(Impl::busMessage), this);
    gst_object_unref(bus);
    
    LOG_DEBUG("WebRTC pipeline created successfully");
    
    // 파이프라인 시작
    GstStateChangeReturn ret = gst_element_set_state(impl_->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Failed to start WebRTC pipeline");
        gst_object_unref(impl_->pipeline);
        impl_->pipeline = nullptr;
        impl_->webrtcbin = nullptr;
        return false;
    }
    
    // 상태 변경 대기
    GstState state, pending;
    ret = gst_element_get_state(impl_->pipeline, &state, &pending, GST_SECOND);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Pipeline state change failed");
        gst_object_unref(impl_->pipeline);
        impl_->pipeline = nullptr;
        impl_->webrtcbin = nullptr;
        return false;
    }
    
    LOG_INFO("WebRTC pipeline started successfully, current state: {}", 
             gst_element_state_get_name(state));
    
    setState(State::CONNECTING);
    
    // negotiation-needed 시그널이 발생하지 않을 수 있으므로 수동으로 offer 생성
    g_idle_add([](gpointer data) -> gboolean {
        auto* peer = static_cast<WebRTCPeer*>(data);
        peer->createOffer();
        return G_SOURCE_REMOVE;
    }, this);
    
    return true;
}

bool WebRTCPeer::createOffer() {
    if (!impl_->webrtcbin) {
        LOG_ERROR("WebRTC not initialized");
        return false;
    }
    
    // 이전 promise가 있으면 정리
    if (impl_->promise) {
        gst_promise_unref(impl_->promise);
        impl_->promise = nullptr;
    }
    
    // Offer 생성
    impl_->promise = gst_promise_new_with_change_func(
        Impl::onOfferCreated, this, nullptr);
    
    g_signal_emit_by_name(impl_->webrtcbin, "create-offer", nullptr, impl_->promise);
    
    LOG_DEBUG("Creating offer for peer: {}", config_.peerId);
    
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
    (void)element;
    auto* peer = static_cast<WebRTCPeer*>(userData);
    LOG_DEBUG("Negotiation needed for peer: {}", peer->config_.peerId);
    
    // 자동으로 Offer 생성
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

void WebRTCPeer::Impl::busMessage(GstBus* bus, GstMessage* message, gpointer userData) {
    auto* peer = static_cast<WebRTCPeer*>(userData);
    
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError* err;
            gchar* debug;
            gst_message_parse_error(message, &err, &debug);
            LOG_ERROR("WebRTC pipeline error: {} ({})", err->message, debug);
            g_error_free(err);
            g_free(debug);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError* err;
            gchar* debug;
            gst_message_parse_warning(message, &err, &debug);
            LOG_WARNING("WebRTC pipeline warning: {} ({})", err->message, debug);
            g_error_free(err);
            g_free(debug);
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(peer->impl_->pipeline)) {
                GstState oldState, newState, pending;
                gst_message_parse_state_changed(message, &oldState, &newState, &pending);
                LOG_TRACE("WebRTC pipeline state: {} -> {}", 
                         gst_element_state_get_name(oldState),
                         gst_element_state_get_name(newState));
            }
            break;
        }
        default:
            break;
    }
}