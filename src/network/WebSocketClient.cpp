#include "network/WebSocketClient.hpp"
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
    
    SoupMessage* msg = soup_message_new(SOUP_METHOD_GET, url.c_str());
    if (!msg) {
        LOG_ERROR("Failed to create message for URL: {}", url);
        return false;
    }

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
        LOG_ERROR("Not connected");
        return;
    }
    
    soup_websocket_connection_send_text(impl_->connection, message.c_str());
}

void WebSocketClient::sendBinary(const std::vector<uint8_t>& data) {
    if (!isConnected()) {
        LOG_ERROR("Not connected");
        return;
    }
    
    GBytes* bytes = g_bytes_new(data.data(), data.size());
    soup_websocket_connection_send_binary(impl_->connection, bytes.data(), bytes.size());
    g_bytes_unref(bytes);
}

void WebSocketClient::onConnected(GObject* source_object, GAsyncResult* res, gpointer user_data) {
    auto* client = static_cast<WebSocketClient*>(user_data);
    
    GError* error = nullptr;
    client->impl_->connection = soup_session_websocket_connect_finish(
        session, result, &error);
    
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

void WebSocketClient::onMessage(SoupWebsocketConnection* conn,
                               SoupWebsocketDataType type,
                               GBytes* message,
                               gpointer userData) {
    auto* client = static_cast<WebSocketClient*>(userData);
    
    if (type == SOUP_WEBSOCKET_DATA_TEXT) {
        gsize size;
        const char* data = static_cast<const char*>(g_bytes_get_data(message, &size));
        std::string msg(data, size);
        
        if (client->messageCallback_) {
            client->messageCallback_(msg);
        }
    }
}

void WebSocketClient::onClosed(SoupWebsocketConnection* conn, gpointer userData) {
    auto* client = static_cast<WebSocketClient*>(userData);
    client->impl_->connected = false;
    
    if (client->disconnectedCallback_) {
        client->disconnectedCallback_();
    }
}