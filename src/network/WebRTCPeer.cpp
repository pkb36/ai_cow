#include "network/WebRTCPeer.hpp"
#include "core/Logger.hpp"
#include <json/json.h>

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
    std::string pipelineStr = fmt::format(
        "webrtcbin name=webrtc bundle-policy=max-bundle "
        "stun-server={} ",
        config_.stunServer
    );
    
    if (config_.useTurn && !config_.turnServer.empty()) {
        pipelineStr += fmt::format("turn-server={} ", config_.turnServer);
    }
    
    GError* error = nullptr;
    impl_->pipeline = gst_parse_launch(pipelineStr.c_str(), &error);
    
    if (error) {
        LOG_ERROR("Failed to create WebRTC pipeline: {}", error->message);
        g_error_free(error);
        return false;
    }
    
    // WebRTC bin 가져오기
    impl_->webrtcbin = gst_bin_get_by_name(GST_BIN(impl_->pipeline), "webrtc");
    if (!impl_->webrtcbin) {
        LOG_ERROR("Failed to get webrtcbin element");
        return false;
    }
    
    // RTP 캡슐화 추가
    GstElement* rtpPay = nullptr;
    if (config_.peerId.find("H264") != std::string::npos) {
        rtpPay = gst_element_factory_make("rtph264pay", nullptr);
    } else if (config_.peerId.find("VP8") != std::string::npos) {
        rtpPay = gst_element_factory_make("rtpvp8pay", nullptr);
    } else {
        rtpPay = gst_element_factory_make("rtph264pay", nullptr); // 기본값
    }
    
    if (!rtpPay) {
        LOG_ERROR("Failed to create RTP payloader");
        return false;
    }
    
    gst_bin_add_many(GST_BIN(impl_->pipeline), udpSrc, rtpPay, nullptr);
    
    // 링크
    if (!gst_element_link(udpSrc, rtpPay)) {
        LOG_ERROR("Failed to link udpsrc to rtppay");
        return false;
    }
    
    // WebRTC에 연결
    GstPad* srcPad = gst_element_get_static_pad(rtpPay, "src");
    GstPad* sinkPad = gst_element_request_pad_simple(impl_->webrtcbin, "sink_%u");
    
    if (gst_pad_link(srcPad, sinkPad) != GST_PAD_LINK_OK) {
        LOG_ERROR("Failed to link RTP to WebRTC");
        gst_object_unref(srcPad);
        gst_object_unref(sinkPad);
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
        return false;
    }
    
    setState(State::CONNECTING);
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
    auto* peer = static_cast<WebRTCPeer*>(userData);
    LOG_DEBUG("Negotiation needed for peer: {}", peer->config_.peerId);
    
    // Offer 생성은 외부에서 명시적으로 호출
}

void WebRTCPeer::Impl::onIceCandidate(GstElement* element, guint mlineIndex, 
                                      gchar* candidate, gpointer userData) {
    auto* peer = static_cast<WebRTCPeer*>(userData);
    
    if (peer->iceCandidateCallback_) {
        peer->iceCandidateCallback_(std::string(candidate), static_cast<int>(mlineIndex));
    }
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

// 통계 정보 가져오기
WebRTCPeer::Statistics WebRTCPeer::getStatistics() const {
    Statistics stats;
    
    if (!impl_->webrtcbin) {
        return stats;
    }
    
    // GstPromise를 사용하여 통계 가져오기
    GstPromise* promise = gst_promise_new();
    g_signal_emit_by_name(impl_->webrtcbin, "get-stats", nullptr, promise);
    
    const GstStructure* reply = gst_promise_wait(promise);
    if (reply) {
        // 통계 파싱 (실제 구현은 WebRTC 통계 구조에 따라 다름)
        // 여기서는 예시로 간단히 구현
        LOG_TRACE("Stats structure: {}", gst_structure_to_string(reply));
    }
    
    gst_promise_unref(promise);
    return stats;
}