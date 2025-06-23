#pragma once

#include <memory>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <nlohmann/json.hpp>  // json/json.h 대신 nlohmann/json.hpp 사용
#include "network/WebRTCPeer.hpp"
#include "video/Pipeline.hpp"

class WebRTCManager {
public:
    struct PeerInfo {
        std::string peerId;
        CameraDevice device;
        StreamType streamType;
        std::chrono::steady_clock::time_point connectedTime;
        WebRTCPeer::State state;
    };

    WebRTCManager(std::shared_ptr<Pipeline> pipeline);
    ~WebRTCManager();

    // Peer 관리
    bool addPeer(const std::string& peerId, const std::string& source);
    bool removePeer(const std::string& peerId);
    void removeAllPeers();

    // 시그널링 메시지 처리
    bool handleOffer(const std::string& peerId, const std::string& sdp);
    bool handleAnswer(const std::string& peerId, const std::string& sdp);
    bool handleIceCandidate(const std::string& peerId, const std::string& candidate, int mlineIndex);

    // 콜백 설정
    using MessageCallback = std::function<void(const std::string& peerId, const std::string& type, const std::string& data)>;
    void setMessageCallback(MessageCallback cb) { messageCallback_ = cb; }

    // 정보 조회
    std::optional<PeerInfo> getPeerInfo(const std::string& peerId) const;
    std::vector<PeerInfo> getAllPeers() const;
    size_t getPeerCount() const;

    // 통계
    struct GlobalStatistics {
        size_t totalPeers = 0;
        size_t activePeers = 0;
        uint64_t totalBytesSent = 0;
        double averageBitrate = 0.0;
    };
    
    GlobalStatistics getGlobalStatistics() const;

private:
    struct PeerContext {
        std::unique_ptr<WebRTCPeer> peer;
        PeerInfo info;
        GstElement* udpSrc = nullptr;
        int streamPort = -1;
    };

    std::shared_ptr<Pipeline> pipeline_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<PeerContext>> peers_;
    MessageCallback messageCallback_;

    // 내부 헬퍼 함수들
    bool createPeerConnection(const std::string& peerId, const std::string& source);
    void setupPeerCallbacks(PeerContext* context);
    CameraDevice parseSource(const std::string& source) const;
    
    // WebRTC 콜백 핸들러들
    void onIceCandidate(const std::string& peerId, const std::string& candidate, int mlineIndex);
    void onOfferCreated(const std::string& peerId, const std::string& sdp);
    void onStateChange(const std::string& peerId, WebRTCPeer::State oldState, WebRTCPeer::State newState);
    void onError(const std::string& peerId, const std::string& error);
};