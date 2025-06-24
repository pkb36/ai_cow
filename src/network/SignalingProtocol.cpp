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
            // 기존 코드와 동일한 형식
            j["peerType"] = "camera";
            j["action"] = "offer";
            j["message"] = {
                {"peer_id", msg.peerId},
                {"sdp", {
                    {"type", "offer"},
                    {"sdp", msg.sdp}
                }}
            };
        },
        [&j](const IceCandidateMessage& msg) {
            // 기존 코드와 동일한 형식
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
        if (!j.contains("message")) return std::nullopt;
        
        auto& msg = j["message"];
        AnswerMessage result;
        
        result.peerId = msg.value("peer_id", "");
        
        // sdp가 객체 형태로 중첩되어 있음
        if (msg.contains("sdp") && msg["sdp"].is_object()) {
            auto& sdpObj = msg["sdp"];
            result.sdp = sdpObj.value("sdp", "");
        } else {
            LOG_ERROR("Invalid SDP format in answer");
            return std::nullopt;
        }
        
        if (result.peerId.empty() || result.sdp.empty()) {
            LOG_ERROR("Missing peer_id or sdp in answer message");
            return std::nullopt;
        }
        
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
        
        // ice가 객체 형태로 중첩되어 있음
        if (msg.contains("ice") && msg["ice"].is_object()) {
            auto& ice = msg["ice"];
            result.candidate = ice.value("candidate", "");
            result.mlineIndex = ice.value("sdpMLineIndex", -1);
        } else {
            LOG_ERROR("Invalid ICE format in candidate message");
            return std::nullopt;
        }
        
        if (result.peerId.empty() || result.candidate.empty() || result.mlineIndex < 0) {
            LOG_ERROR("Invalid ICE candidate message");
            return std::nullopt;
        }
        
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