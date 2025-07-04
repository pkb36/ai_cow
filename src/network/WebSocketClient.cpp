#include "network/WebSocketClient.hpp"
#include "core/Application.hpp"
#include "core/Logger.hpp"

struct WebSocketClient::Impl {
    SoupSession* session = nullptr;
    SoupWebsocketConnection* connection = nullptr;
    std::string url;
    bool connected = false;
};

WebSocketClient::WebSocketClient() : impl_(std::make_unique<Impl>()) {
    impl_->session = soup_session_new();
}

WebSocketClient::~WebSocketClient() {
    disconnect();
    if (impl_->session) {
        g_object_unref(impl_->session);
    }
}

bool WebSocketClient::connect(const std::string& url) {
    if (impl_->connected) {
        LOG_WARNING("Already connected");
        return true;
    }

    impl_->url = url;
    
    // WebSocket 전용 세션 생성
    const char* https_aliases[] = {"wss", NULL};
    
    if (impl_->session) {
        g_object_unref(impl_->session);
    }
    
    // 특정 컨텍스트를 사용하도록 설정
    GMainContext* wsContext = Application::getInstance().getWebSocketContext();
    
    impl_->session = soup_session_new_with_options(
        SOUP_SESSION_SSL_STRICT, FALSE,
        SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE, TRUE,
        SOUP_SESSION_HTTPS_ALIASES, https_aliases,
        SOUP_SESSION_ASYNC_CONTEXT, wsContext,  // 중요: WebSocket 전용 컨텍스트 사용
        NULL);
    
    SoupMessage* msg = soup_message_new(SOUP_METHOD_GET, url.c_str());
    if (!msg) {
        LOG_ERROR("Failed to create message for URL: {}", url);
        return false;
    }

    LOG_INFO("Attempting WebSocket connection to: {}", url);

    soup_session_websocket_connect_async(
        impl_->session, msg, nullptr, nullptr, nullptr,
        &WebSocketClient::onConnected,
        this
    );

    return true;
}

void WebSocketClient::disconnect() {
    if (impl_->connection) {
        if (soup_websocket_connection_get_state(impl_->connection) == SOUP_WEBSOCKET_STATE_OPEN) {
            soup_websocket_connection_close(impl_->connection, 1000, "Client disconnect");
        }
        g_object_unref(impl_->connection);
        impl_->connection = nullptr;
    }
    impl_->connected = false;
}

bool WebSocketClient::isConnected() const {
    return impl_->connected && impl_->connection &&
           soup_websocket_connection_get_state(impl_->connection) == SOUP_WEBSOCKET_STATE_OPEN;
}

void WebSocketClient::sendText(const std::string& message) {
    if (!isConnected()) {
        LOG_ERROR("Cannot send - WebSocket not connected!");
        return;
    }
    
    LOG_TRACE("Sending WebSocket message: {}", 
              message.length() > 200 ? message.substr(0, 200) + "..." : message);
    
    soup_websocket_connection_send_text(impl_->connection, message.c_str());
}

void WebSocketClient::sendBinary(const std::vector<uint8_t>& data) {
    if (!isConnected()) {
        LOG_ERROR("Not connected");
        return;
    }
    
    // libsoup-2.4에서는 3개의 인자가 필요: connection, data, size
    soup_websocket_connection_send_binary(impl_->connection, data.data(), data.size());
}

void WebSocketClient::onConnected(GObject* source_object, GAsyncResult* result, gpointer userData) {
    auto* client = static_cast<WebSocketClient*>(userData);
    
    GError* error = nullptr;
    client->impl_->connection = soup_session_websocket_connect_finish(
        SOUP_SESSION(source_object), result, &error);
    
    if (error) {
        LOG_ERROR("WebSocket connection failed: {}", error->message);
        g_error_free(error);
        if (client->disconnectedCallback_) {
            client->disconnectedCallback_();
        }
        return;
    }
    
    client->impl_->connected = true;
    
    // 시그널 연결
    g_signal_connect(client->impl_->connection, "message",
                     G_CALLBACK(&WebSocketClient::onMessage), client);
    g_signal_connect(client->impl_->connection, "closed",
                     G_CALLBACK(&WebSocketClient::onClosed), client);
    
    if (client->connectedCallback_) {
        client->connectedCallback_();
    }
}
    
void WebSocketClient::onMessage(SoupWebsocketConnection* /*conn*/,
                               SoupWebsocketDataType type,
                               GBytes* message,
                               gpointer userData) {
    auto* client = static_cast<WebSocketClient*>(userData);

    LOG_INFO("=== WebSocketClient::onMessage CALLED ===");
    LOG_INFO("Message type: {}", (type == SOUP_WEBSOCKET_DATA_TEXT) ? "TEXT" : "BINARY");
    
    if (type == SOUP_WEBSOCKET_DATA_TEXT) {
        gsize size;
        const char* data = static_cast<const char*>(g_bytes_get_data(message, &size));
        std::string msg(data, size);
        
        if (client->messageCallback_) {
            client->messageCallback_(msg);
        }
    }
}

void WebSocketClient::onClosed(SoupWebsocketConnection* /*conn*/, gpointer userData) {
    auto* client = static_cast<WebSocketClient*>(userData);
    client->impl_->connected = false;
    
    if (client->disconnectedCallback_) {
        client->disconnectedCallback_();
    }
}