#pragma once

#include <string>
#include <optional>
#include <nlohmann/json.hpp>
#include <variant>

// 시그널링 메시지 타입들
namespace Signaling {

// 기본 메시지 타입
struct RegisterMessage {
    std::string peerType = "camera";
    std::string cameraId;
    std::string firmwareVersion;
    std::string aiVersion;
};

struct CameraStatusMessage {
    std::string recordStatus;
    int recordUsage;
    int cpuTemp;
    int gpuTemp;
    std::string rgbSnapshot;
    std::string thermalSnapshot;
};

struct PeerJoinedMessage {
    std::string peerId;
    std::string source;
};

struct PeerLeftMessage {
    std::string peerId;
};

struct OfferMessage {
    std::string peerId;
    std::string sdp;
};

struct AnswerMessage {
    std::string peerId;
    std::string sdp;
};

struct IceCandidateMessage {
    std::string peerId;
    std::string candidate;
    int mlineIndex;
};

// 명령 메시지들
struct CommandMessage {
    std::string peerId;
    std::string command;
    nlohmann::json parameters;
};

// 모든 메시지 타입을 포함하는 variant
using Message = std::variant<
    RegisterMessage,
    CameraStatusMessage,
    PeerJoinedMessage,
    PeerLeftMessage,
    OfferMessage,
    AnswerMessage,
    IceCandidateMessage,
    CommandMessage
>;

// 메시지 파싱 및 생성
class MessageParser {
public:
    static std::optional<Message> parse(const std::string& jsonStr);
    static std::string serialize(const Message& message);
    
private:
   static std::optional<RegisterMessage> parseRegister(const nlohmann::json& j);
   static std::optional<PeerJoinedMessage> parsePeerJoined(const nlohmann::json& j);
   static std::optional<AnswerMessage> parseAnswer(const nlohmann::json& j);
   static std::optional<IceCandidateMessage> parseIceCandidate(const nlohmann::json& j);
   static std::optional<CommandMessage> parseCommand(const nlohmann::json& j);
};

// 메시지 방문자 패턴
template<typename... Ts>
struct MessageVisitor : Ts... { 
   using Ts::operator()...; 
};

template<typename... Ts>
MessageVisitor(Ts...) -> MessageVisitor<Ts...>;

} // namespace Signaling