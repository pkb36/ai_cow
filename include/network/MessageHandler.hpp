#pragma once

#include <functional>
#include <memory>
#include "network/SignalingProtocol.hpp"
#include "network/WebRTCManager.hpp"

class MessageHandler {
public:
    MessageHandler(std::shared_ptr<WebRTCManager> webrtcManager);
    ~MessageHandler() = default;

    // 메시지 처리
    void handleMessage(const std::string& message);
    
    // 메시지 전송 콜백
    using SendMessageCallback = std::function<void(const std::string&)>;
    void setSendMessageCallback(SendMessageCallback cb) { sendCallback_ = cb; }

    // 상태 메시지 전송
    void sendRegistration(const std::string& cameraId);
    void sendCameraStatus(const Signaling::CameraStatusMessage& status);
    void sendOffer(const std::string& peerId, const std::string& sdp);
    void sendIceCandidate(const std::string& peerId, const std::string& candidate, int mlineIndex);

private:
    std::shared_ptr<WebRTCManager> webrtcManager_;
    SendMessageCallback sendCallback_;
    
    // 각 메시지 타입별 핸들러
    void handlePeerJoined(const Signaling::PeerJoinedMessage& msg);
    void handlePeerLeft(const Signaling::PeerLeftMessage& msg);
    void handleAnswer(const Signaling::AnswerMessage& msg);
    void handleIceCandidate(const Signaling::IceCandidateMessage& msg);
    void handleCommand(const Signaling::CommandMessage& msg);
    void handleRegister(const Signaling::RegisterMessage& msg);
    void handleCameraStatus(const Signaling::CameraStatusMessage& msg);
    void handleOffer(const Signaling::OfferMessage& msg);


    // 명령 처리기
    void processPtzCommand(const std::string& peerId, const nlohmann::json& params);
    void processRecordCommand(const std::string& peerId, const nlohmann::json& params);
    void processCustomCommand(const std::string& peerId, const nlohmann::json& params);
};