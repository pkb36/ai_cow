#pragma once

#include <memory>
#include <string>
#include <functional>
#include <optional>
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>

class WebRTCPeer {
public:
    enum class State {
        NEW,
        CONNECTING,
        CONNECTED,
        FAILED,
        CLOSED
    };

    struct Config {
        std::string peerId;
        std::string stunServer = "stun://stun.l.google.com:19302";
        std::string turnServer;
        std::string turnUser;
        std::string turnPassword;
        bool useTurn = false;
    };

    // 콜백 타입들
    using IceCandidateCallback = std::function<void(const std::string& candidate, int mlineIndex)>;
    using OfferCreatedCallback = std::function<void(const std::string& sdp)>;
    using StateChangeCallback = std::function<void(State oldState, State newState)>;
    using ErrorCallback = std::function<void(const std::string& error)>;

    WebRTCPeer(const Config& config);
    ~WebRTCPeer();

    // 연결 관리
    bool createOffer();
    bool setRemoteDescription(const std::string& type, const std::string& sdp);
    bool addIceCandidate(const std::string& candidate, int mlineIndex);

    // 스트림 연결
    bool connectToStream(GstElement* udpSrc);
    void disconnect();

    // 콜백 설정
    void setIceCandidateCallback(IceCandidateCallback cb) { iceCandidateCallback_ = cb; }
    void setOfferCreatedCallback(OfferCreatedCallback cb) { offerCreatedCallback_ = cb; }
    void setStateChangeCallback(StateChangeCallback cb) { stateChangeCallback_ = cb; }
    void setErrorCallback(ErrorCallback cb) { errorCallback_ = cb; }

    // 상태 조회
    State getState() const { return state_; }
    const std::string& getPeerId() const { return config_.peerId; }
    bool isConnected() const { return state_ == State::CONNECTED; }

    // 통계 정보
    struct Statistics {
        uint64_t bytesSent = 0;
        uint64_t bytesReceived = 0;
        uint64_t packetsSent = 0;
        uint64_t packetsReceived = 0;
        double bitrate = 0.0;
        double packetLoss = 0.0;
        double roundTripTime = 0.0;
    };
    
    Statistics getStatistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    
    Config config_;
    State state_ = State::NEW;
    
    // 콜백들
    IceCandidateCallback iceCandidateCallback_;
    OfferCreatedCallback offerCreatedCallback_;
    StateChangeCallback stateChangeCallback_;
    ErrorCallback errorCallback_;
    
    void setState(State newState);
};