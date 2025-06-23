#pragma once

#include <memory>
#include <functional>
#include <string>
#include <libsoup/soup.h>
#include <glib.h>

class WebSocketClient {
public:
    using MessageCallback = std::function<void(const std::string&)>;
    using ConnectedCallback = std::function<void()>;
    using DisconnectedCallback = std::function<void()>;

    WebSocketClient();
    ~WebSocketClient();

    // 콜백 설정
    void setMessageCallback(MessageCallback cb) { messageCallback_ = cb; }
    void setConnectedCallback(ConnectedCallback cb) { connectedCallback_ = cb; }
    void setDisconnectedCallback(DisconnectedCallback cb) { disconnectedCallback_ = cb; }

    // 연결 관리
    bool connect(const std::string& url);
    void disconnect();
    bool isConnected() const;

    // 메시지 전송
    void sendText(const std::string& message);
    void sendBinary(const std::vector<uint8_t>& data);

private:
    // Soup 콜백들 (static 함수로)
    static void onConnected(SoupSession* session, GAsyncResult* res, gpointer userData);
    static void onMessage(SoupWebsocketConnection* conn, SoupWebsocketDataType type, 
                         GBytes* message, gpointer userData);
    static void onClosed(SoupWebsocketConnection* conn, gpointer userData);

    // 내부 구현
    struct Impl;
    std::unique_ptr<Impl> impl_;

    MessageCallback messageCallback_;
    ConnectedCallback connectedCallback_;
    DisconnectedCallback disconnectedCallback_;
};