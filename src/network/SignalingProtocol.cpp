#include "network/SignalingProtocol.hpp"
#include "core/Logger.hpp"

namespace Signaling {

std::optional<Message> MessageParser::parse(const std::string& jsonStr) {
    try {
        auto j = nlohmann::json::parse(jsonStr);
        
        if (!j.contains("action")) {
            LOG_ERROR("Missing 'action' field in message");
            return std::nullopt;
        }
        
        std::string action = j["action"];

        if (action == "camstatus_reply") {
            LOG_DEBUG("Ignoring camstatus_reply");
            return std::nullopt;
        }
        
        if (action == "ROOM_PEER_JOINED") {
            return parsePeerJoined(j);
        } else if (action == "ROOM_PEER_LEFT") {
            return parsePeerLeft(j);
        } else if (action == "answer") {
            return parseAnswer(j);
        } else if (action == "candidate") {
            return parseIceCandidate(j);
        } else if (action == "send_camera") {
            return parseCommand(j);
        }
        
        LOG_WARNING("Unknown action: {}", action);
        return std::nullopt;
        
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR("JSON parsing error: {}", e.what());
        return std::nullopt;
    }
}

std::string MessageParser::serialize(const Message& message) {
    nlohmann::json j;
    
    std::visit(MessageVisitor{
        [&j](const RegisterMessage& msg) {
            j["peerType"] = msg.peerType;
            j["action"] = "register";
            j["message"] = {
                {"name", msg.cameraId},
                {"fw_version", msg.firmwareVersion},
                {"ai_version", msg.aiVersion}
            };
        },
        [&j](const CameraStatusMessage& msg) {
            j["peerType"] = "camera";
            j["action"] = "camstatus";
            j["message"] = {
                {"rec_status", msg.recordStatus},
                {"rec_usage", msg.recordUsage},
                {"cpu_temp", msg.cpuTemp},
                {"gpu_temp", msg.gpuTemp},
                {"rgb_snapshot", msg.rgbSnapshot},
                {"thermal_snapshot", msg.thermalSnapshot}
            };
        },
        [&j](const OfferMessage& msg) {
            j["peerType"] = "camera";
            j["action"] = "offer";
            j["message"] = {
                {"peer_id", msg.peerId},
                {"sdp", msg.sdp}  // JSON 객체 그대로 사용
            };
        },
        [&j](const IceCandidateMessage& msg) {
            j["peerType"] = "camera";
            j["action"] = "candidate";
            j["message"] = {
                {"peer_id", msg.peerId},
                {"ice", {
                    {"candidate", msg.candidate},
                    {"sdpMLineIndex", msg.mlineIndex}
                }}
            };
        },
        [&j](const PeerJoinedMessage& msg) {
            j["peerType"] = "client";
            j["action"] = "peer_joined";
            j["message"] = {
                {"peer_id", msg.peerId},
                {"source", msg.source}
            };
        },
        [&j](const PeerLeftMessage& msg) {
            j["peerType"] = "client";
            j["action"] = "peer_left";
            j["message"] = {
                {"peer_id", msg.peerId}
            };
        },
        [&j](const AnswerMessage& msg) {
            j["peerType"] = "client";
            j["action"] = "answer";
            j["message"] = {
                {"peer_id", msg.peerId},
                {"sdp", msg.sdp}
            };
        },
        [&j](const CommandMessage& msg) {
            j["peerType"] = "controller";
            j["action"] = "command";
            j["message"] = {
                {"peer_id", msg.peerId},
                {"command", msg.command}
            };
        },
        [&j](const auto& msg) {
            // 기타 메시지 타입들
            j["peerType"] = "camera";
            j["action"] = "unknown";
        }
    }, message);
    
    return j.dump();
}

std::optional<PeerJoinedMessage> MessageParser::parsePeerJoined(const nlohmann::json& j) {
    try {
        if (!j.contains("message")) return std::nullopt;
        
        auto& msg = j["message"];
        PeerJoinedMessage result;
        
        result.peerId = msg.value("peer_id", "");
        result.source = msg.value("source", "RGB");
        
        if (result.peerId.empty()) {
            LOG_ERROR("Missing peer_id in ROOM_PEER_JOINED message");
            return std::nullopt;
        }
        
        return result;
    } catch (const std::exception& e) {
        LOG_ERROR("Error parsing PeerJoined message: {}", e.what());
        return std::nullopt;
    }
}

std::optional<PeerLeftMessage> MessageParser::parsePeerLeft(const nlohmann::json& j)
{
    try {
        if (!j.contains("message")) return std::nullopt;

        auto& msg = j["message"];
        PeerLeftMessage result;

        result.peerId = msg.value("peer_id", "");

        if (result.peerId.empty()) {
            LOG_ERROR("Missing peer_id in ROOM_PEER_LEFT message");
            return std::nullopt;
        }

        return result;
    } catch (const std::exception& e) {
        LOG_ERROR("Error parsing PeerLeft message: {}", e.what());
        return std::nullopt;
    }
}

std::optional<AnswerMessage> MessageParser::parseAnswer(const nlohmann::json& j) {
    try {
        if (!j.contains("message")) {
            LOG_ERROR("No 'message' field in answer");
            return std::nullopt;
        }

        auto& msg = j["message"];
        AnswerMessage result;

        // peer_id 파싱 - message 내부에 있음
        result.peerId = msg.value("peer_id", "");
        
        // SDP 파싱 - message.sdp 내부에 있음
        if (msg.contains("sdp")) {
            if (msg["sdp"].is_object()) {
                // sdp가 객체인 경우 (현재 웹 클라이언트가 보내는 형식)
                result.sdp = msg["sdp"];
                LOG_DEBUG("SDP is object type");
            } else if (msg["sdp"].is_string()) {
                // sdp가 문자열인 경우
                try {
                    result.sdp = nlohmann::json::parse(msg["sdp"].get<std::string>());
                } catch (...) {
                    // 파싱 실패 시 빈 객체로
                    result.sdp = nlohmann::json::object();
                    result.sdp["sdp"] = msg["sdp"].get<std::string>();
                    result.sdp["type"] = "answer";
                }
                LOG_DEBUG("SDP is string type");
            }
        } else {
            LOG_ERROR("No 'sdp' field in answer message");
            return std::nullopt;
        }

        if (result.peerId.empty()) {
            LOG_ERROR("Empty peer_id in answer message");
            return std::nullopt;
        }

        if (result.sdp.empty()) {
            LOG_ERROR("Empty sdp in answer message");
            return std::nullopt;
        }

        LOG_INFO("Successfully parsed answer from peer: {}", result.peerId);
        return result;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error parsing Answer message: {}", e.what());
        return std::nullopt;
    }
}

std::optional<IceCandidateMessage> MessageParser::parseIceCandidate(const nlohmann::json& j) {
    try {
        if (!j.contains("message")) return std::nullopt;

        auto& msg = j["message"];
        IceCandidateMessage result;

        result.peerId = msg.value("peer_id", "");
        
        if (msg.contains("ice")) {
            auto& ice = msg["ice"];
            result.candidate = ice.value("candidate", "");
            result.mlineIndex = ice.value("sdpMLineIndex", -1);
            
            // sdpMid 필드도 있을 수 있음 (필요시 처리)
            // std::string sdpMid = ice.value("sdpMid", "");
        } else {
            LOG_ERROR("Missing 'ice' field in candidate message");
            return std::nullopt;
        }

        if (result.peerId.empty() || result.candidate.empty() || result.mlineIndex < 0) {
            LOG_ERROR("Invalid ICE candidate message - peerId: {}, candidate: {}, mlineIndex: {}", 
                      result.peerId, 
                      result.candidate.empty() ? "empty" : "present", 
                      result.mlineIndex);
            return std::nullopt;
        }

        LOG_DEBUG("Parsed ICE candidate for peer: {}, mlineIndex: {}", 
                  result.peerId, result.mlineIndex);

        return result;
    } catch (const std::exception& e) {
        LOG_ERROR("Error parsing IceCandidate message: {}", e.what());
        return std::nullopt;
    }
}

std::optional<CommandMessage> MessageParser::parseCommand(const nlohmann::json& j) {
    try {
        if (!j.contains("message")) return std::nullopt;
        
        auto& msg = j["message"];
        CommandMessage result;
        
        result.peerId = msg.value("peer_id", "");
        
        // 다양한 명령 타입 처리
        if (msg.contains("ptz")) {
            result.command = "ptz";
            result.parameters = msg["ptz"];
        } else if (msg.contains("record")) {
            result.command = "record";
            result.parameters = msg["record"];
        } else if (msg.contains("custom_command")) {
            result.command = "custom_command";
            result.parameters = msg;
        }
        // ... 더 많은 명령 타입들
        
        return result;
    } catch (const std::exception& e) {
        LOG_ERROR("Error parsing Command message: {}", e.what());
        return std::nullopt;
    }
}

} // namespace Signaling