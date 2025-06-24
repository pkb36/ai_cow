#include "network/MessageHandler.hpp"
#include "core/Logger.hpp"
#include "core/Config.hpp"

MessageHandler::MessageHandler(std::shared_ptr<WebRTCManager> webrtcManager)
    : webrtcManager_(webrtcManager) {
    
    // WebRTC 매니저의 메시지 콜백 설정
    webrtcManager_->setMessageCallback(
        [this](const std::string& peerId, const std::string& type, const std::string& data) {
            if (type == "offer") {
                sendOffer(peerId, data);
            } else if (type == "candidate") {
                // data는 이미 JSON 형식
                auto j = nlohmann::json::parse(data);
                sendIceCandidate(peerId, j["candidate"], j["mlineIndex"]);
            }
        }
    );
}

void MessageHandler::handleMessage(const std::string& message) {
    auto parsedMsg = Signaling::MessageParser::parse(message);
    if (!parsedMsg) {
        LOG_WARNING("Failed to parse message: {}", message);
        return;
    }
    
    // Visitor 패턴을 사용한 메시지 디스패치
    std::visit(Signaling::MessageVisitor{
        [this](const Signaling::PeerJoinedMessage& msg) { handlePeerJoined(msg); },
        [this](const Signaling::PeerLeftMessage& msg) { handlePeerLeft(msg); },
        [this](const Signaling::AnswerMessage& msg) { handleAnswer(msg); },
        [this](const Signaling::IceCandidateMessage& msg) { handleIceCandidate(msg); },
        [this](const Signaling::CommandMessage& msg) { handleCommand(msg); },
        [](const auto&) {  // 매개변수 이름 제거
            LOG_WARNING("Unhandled message type");
        }
    }, *parsedMsg);
}

void MessageHandler::handlePeerJoined(const Signaling::PeerJoinedMessage& msg) {
    LOG_INFO("Peer joined: {} with source: {}", msg.peerId, msg.source);
    
    if (!webrtcManager_->addPeer(msg.peerId, msg.source)) {
        LOG_ERROR("Failed to add peer: {}", msg.peerId);
        return;
    }
    
    // Offer 생성은 WebRTCManager 내부에서 처리
}

void MessageHandler::handlePeerLeft(const Signaling::PeerLeftMessage& msg) {
    LOG_INFO("Peer left: {}", msg.peerId);
    webrtcManager_->removePeer(msg.peerId);
}

void MessageHandler::handleAnswer(const Signaling::AnswerMessage& msg) {
    LOG_DEBUG("Received answer from peer: {}", msg.peerId);
    webrtcManager_->handleAnswer(msg.peerId, msg.sdp);
}

void MessageHandler::handleIceCandidate(const Signaling::IceCandidateMessage& msg) {
    LOG_TRACE("Received ICE candidate from peer: {}", msg.peerId);
    webrtcManager_->handleIceCandidate(msg.peerId, msg.candidate, msg.mlineIndex);
}

void MessageHandler::handleCommand(const Signaling::CommandMessage& msg) {
    LOG_INFO("Received command '{}' from peer: {}", msg.command, msg.peerId);
    
    if (msg.command == "ptz") {
        processPtzCommand(msg.peerId, msg.parameters);
    } else if (msg.command == "record") {
        processRecordCommand(msg.peerId, msg.parameters);
    } else if (msg.command == "custom_command") {
        processCustomCommand(msg.peerId, msg.parameters);
    } else {
        LOG_WARNING("Unknown command: {}", msg.command);
    }
}

void MessageHandler::handleOffer(const Signaling::OfferMessage& msg)
{
    LOG_INFO("Received OfferMessage from peer: {}", msg.peerId);
}

void MessageHandler::sendRegistration(const std::string& cameraId) {
    Signaling::RegisterMessage msg;
    msg.cameraId = cameraId;
    msg.firmwareVersion = "1.0.0";  // 실제 버전 정보로 대체
    msg.aiVersion = "0.1.0";
    
    auto jsonStr = Signaling::MessageParser::serialize(msg);
    
    if (sendCallback_) {
        sendCallback_(jsonStr);
    }
}

void MessageHandler::sendCameraStatus(const Signaling::CameraStatusMessage& status) {
    auto jsonStr = Signaling::MessageParser::serialize(status);
    
    if (sendCallback_) {
        sendCallback_(jsonStr);
    }
}

void MessageHandler::sendOffer(const std::string& peerId, const std::string& sdp) {
    Signaling::OfferMessage msg;
    msg.peerId = peerId;
    msg.sdp = sdp;
    
    auto jsonStr = Signaling::MessageParser::serialize(msg);
    
    if (sendCallback_) {
        sendCallback_(jsonStr);
    }
}

void MessageHandler::sendIceCandidate(const std::string& peerId, 
                                     const std::string& candidate, 
                                     int mlineIndex) {
    Signaling::IceCandidateMessage msg;
    msg.peerId = peerId;
    msg.candidate = candidate;
    msg.mlineIndex = mlineIndex;
    
    auto jsonStr = Signaling::MessageParser::serialize(msg);
    
    if (sendCallback_) {
        sendCallback_(jsonStr);
    }
}

// 명령 처리 예시
void MessageHandler::processPtzCommand(const std::string& peerId, 
                                      const nlohmann::json& params) {
    try {
        std::string ptzData = params.get<std::string>();
        LOG_INFO("PTZ command from {}: {}", peerId, ptzData);
        
        // PTZ 제어 로직
        // SerialPort::getInstance().send(ptzData);
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error processing PTZ command: {}", e.what());
    }
}

// 명령 처리 예시
void MessageHandler::processRecordCommand(const std::string& peerId, 
                                      const nlohmann::json& params) {
    try {
        std::string ptzData = params.get<std::string>();
        LOG_INFO("Record command from {}: {}", peerId, ptzData);
        
        // PTZ 제어 로직
        // SerialPort::getInstance().send(ptzData);
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error processing Record command: {}", e.what());
    }
}

// 명령 처리 예시
void MessageHandler::processCustomCommand(const std::string& peerId, 
                                      const nlohmann::json& params) {
    try {
        std::string ptzData = params.get<std::string>();
        LOG_INFO("Custom command from {}: {}", peerId, ptzData);
        
        // PTZ 제어 로직
        // SerialPort::getInstance().send(ptzData);
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error processing Custom command: {}", e.what());
    }
}