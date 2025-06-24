#include "network/MessageHandler.hpp"
#include "core/Logger.hpp"
#include "core/Config.hpp"

MessageHandler::MessageHandler(std::shared_ptr<WebRTCManager> webrtcManager)
    : webrtcManager_(webrtcManager),
    backgroundTasks_(std::make_unique<ThreadPool>(2)) {
    
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
    // 원본 메시지 로깅 (디버깅용)
    try {
        auto j = nlohmann::json::parse(message);
        std::string action = j.value("action", "unknown");
        
        // answer나 candidate 메시지는 전체 구조를 로깅
        if (action == "answer" || action == "candidate") {
            LOG_DEBUG("Raw {} message structure: {}", action, j.dump(2));
        }
    } catch (...) {
        // JSON 파싱 실패 무시
    }
    
    auto parsedMsg = Signaling::MessageParser::parse(message);
    if (!parsedMsg) {
        LOG_WARNING("Failed to parse message: {}", message);
        return;
    }
    
    // Visitor 패턴을 사용한 메시지 디스패치
    std::visit(Signaling::MessageVisitor{
        [this](const Signaling::PeerJoinedMessage& msg) { handlePeerJoined(msg); },
        [this](const Signaling::PeerLeftMessage& msg) { handlePeerLeft(msg); },
        [this](const Signaling::AnswerMessage& msg) { 
            LOG_INFO("Answer message received and parsed successfully for peer: {}", msg.peerId);
            handleAnswer(msg); 
        },
        [this](const Signaling::IceCandidateMessage& msg) { 
            LOG_DEBUG("ICE candidate received and parsed successfully for peer: {}", msg.peerId);
            handleIceCandidate(msg); 
        },
        [this](const Signaling::CommandMessage& msg) { handleCommand(msg); },
        [](const auto&) {
            LOG_WARNING("Unhandled message type");
        }
    }, *parsedMsg);
}

void MessageHandler::handlePeerJoined(const Signaling::PeerJoinedMessage& msg) {
    LOG_INFO("Peer joined: {} with source: {}", msg.peerId, msg.source);
    
    // 완전 비동기 처리 - 메인 스레드 블로킹 방지
    backgroundTasks_->enqueue([this, peerId = msg.peerId, source = msg.source]() {
        LOG_INFO("Processing peer connection asynchronously: {}", peerId);
        
        try {
            // 비동기로 peer 추가
            if (!webrtcManager_->addPeerAsync(peerId, source)) {
                LOG_ERROR("Failed to add peer: {}", peerId);
                return;
            }
            
            LOG_INFO("Peer {} added successfully in background", peerId);
            
        } catch (const std::exception& e) {
            LOG_ERROR("Exception adding peer {}: {}", peerId, e.what());
        }
    });
    
    // 즉시 반환하여 다른 메시지 처리 가능
    LOG_DEBUG("Peer {} queued for background processing", msg.peerId);
}

void MessageHandler::handlePeerLeft(const Signaling::PeerLeftMessage& msg) {
    LOG_INFO("Peer left: {}", msg.peerId);
    webrtcManager_->removePeer(msg.peerId);
}

void MessageHandler::handleAnswer(const Signaling::AnswerMessage& msg) {
    LOG_INFO("=== Handling answer from peer: {} ===", msg.peerId);
    
    std::string actualPeerId = msg.peerId;
    // JSON에서 SDP 문자열 추출
    std::string sdpStr;
    
    try {
        if (msg.sdp.is_object()) {
            // sdp가 객체인 경우 {"type": "answer", "sdp": "v=0..."}
            if (msg.sdp.contains("sdp")) {
                if (msg.sdp["sdp"].is_string()) {
                    sdpStr = msg.sdp["sdp"].get<std::string>();
                    LOG_DEBUG("Extracted SDP from object, length: {}", sdpStr.length());
                }
            }
        } else if (msg.sdp.is_string()) {
            // sdp가 문자열인 경우
            sdpStr = msg.sdp.get<std::string>();
            LOG_DEBUG("SDP is string, length: {}", sdpStr.length());
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to extract SDP: {}", e.what());
        return;
    }
    
    if (!sdpStr.empty()) {
        LOG_INFO("Forwarding answer to WebRTC manager, SDP starts with: {}", 
                 sdpStr.substr(0, 50));
        webrtcManager_->handleAnswer(actualPeerId, sdpStr);
    } else {
        LOG_ERROR("Failed to extract SDP string from answer message");
        LOG_DEBUG("SDP object dump: {}", msg.sdp.dump());
    }
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
    msg.cameraId = "ITC100A-23081111";
    msg.firmwareVersion = "1.0.0";  // 실제 버전 정보로 대체
    msg.aiVersion = "0.1.0";
    
    auto jsonStr = Signaling::MessageParser::serialize(msg);

    LOG_INFO("=== Sending Registration ===");
    LOG_INFO("Camera ID: {}", msg.cameraId);
    LOG_DEBUG("Registration message: {}", jsonStr);
    
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
    // SDP JSON 객체 생성
    nlohmann::json sdpObj;
    sdpObj["type"] = "offer";
    sdpObj["sdp"] = sdp;  // 원본 SDP 문자열
    
    Signaling::OfferMessage msg;
    msg.peerId = peerId;
    msg.sdp = sdpObj;  // JSON 객체 그대로 저장
    
    auto jsonStr = Signaling::MessageParser::serialize(msg);
    
    LOG_DEBUG("Sending offer for peer {}", peerId);
    LOG_TRACE("Offer message: {}", jsonStr);
    
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
    
    LOG_DEBUG("Sending ICE candidate {} for peer {}", mlineIndex, peerId);
    LOG_TRACE("ICE candidate message: {}", jsonStr);
    
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