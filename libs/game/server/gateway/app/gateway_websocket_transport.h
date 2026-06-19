#ifndef YUAN_GAME_SERVER_GATEWAY_APP_GATEWAY_WEBSOCKET_TRANSPORT_H
#define YUAN_GAME_SERVER_GATEWAY_APP_GATEWAY_WEBSOCKET_TRANSPORT_H

#include "common/rpc_network.h"
#include "websocket.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace yuan::game::server
{
    class GatewayWebSocketTransport final : public yuan::net::websocket::WebSocketDataHandler
    {
    public:
        GatewayWebSocketTransport(yuan::rpc::Server &server, yuan::net::NetworkRuntime &runtime);
        ~GatewayWebSocketTransport();

        GatewayWebSocketTransport(const GatewayWebSocketTransport &) = delete;
        GatewayWebSocketTransport &operator=(const GatewayWebSocketTransport &) = delete;

        bool start(std::uint16_t port, std::size_t max_buffered_bytes);
        void serve();
        void stop();

        bool write_message_to_connection(std::uint64_t connection_id, const yuan::rpc::Message &message);
        bool write_response_to_connection(std::uint64_t connection_id, yuan::rpc::Response response);
        bool close_connection(std::uint64_t connection_id);
        void close_all_connections();
        [[nodiscard]] std::size_t active_connection_count() const;
        void set_connection_closed_callback(std::function<void(std::uint64_t)> callback);

        void on_connected(yuan::net::websocket::WebSocketConnection *wsConn) override;
        void on_data(yuan::net::websocket::WebSocketConnection *wsConn, const yuan::buffer::ByteBuffer &buff) override;
        void on_close(yuan::net::websocket::WebSocketConnection *wsConn) override;

    private:
        [[nodiscard]] std::uint64_t connection_id_for(yuan::net::websocket::WebSocketConnection *wsConn) const;
        bool write_frame(std::uint64_t connection_id, yuan::rpc::Bytes frame);

        yuan::net::NetworkRuntime &runtime_;
        yuan::net::websocket::WebSocketServer server_;
        rpc_network::RpcFrameConnectionDispatcher dispatcher_;
        std::function<void(std::uint64_t)> connection_closed_callback_;
        mutable std::mutex mutex_;
        std::unordered_map<yuan::net::websocket::WebSocketConnection *, std::uint64_t> ids_by_connection_;
        std::unordered_map<std::uint64_t, yuan::net::websocket::WebSocketConnection *> connections_by_id_;
        std::atomic<std::uint64_t> next_connection_id_{10'000'000};
        bool started_ = false;
    };
}

#endif
