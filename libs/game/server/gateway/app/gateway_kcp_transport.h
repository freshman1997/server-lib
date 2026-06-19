#ifndef YUAN_GAME_SERVER_GATEWAY_APP_GATEWAY_KCP_TRANSPORT_H
#define YUAN_GAME_SERVER_GATEWAY_APP_GATEWAY_KCP_TRANSPORT_H

#include "common/rpc_network.h"
#include "net/session/kcp_server_session.h"

#include <cstdint>
#include <functional>

namespace yuan::game::server
{
    class GatewayKcpTransport final
    {
    public:
        static constexpr std::uint8_t kHandshakePacket = 1;
        static constexpr std::uint8_t kHandshakeAckPacket = 2;
        static constexpr std::uint8_t kKcpPacket = 3;

        GatewayKcpTransport(yuan::rpc::Server &server, yuan::net::NetworkRuntime &runtime);
        ~GatewayKcpTransport();

        GatewayKcpTransport(const GatewayKcpTransport &) = delete;
        GatewayKcpTransport &operator=(const GatewayKcpTransport &) = delete;

        bool start(yuan::net::KcpServerSession::Config config, std::size_t max_buffered_bytes);
        void stop();

        bool write_message_to_connection(std::uint64_t connection_id, const yuan::rpc::Message &message);
        bool write_response_to_connection(std::uint64_t connection_id, yuan::rpc::Response response);
        bool close_connection(std::uint64_t connection_id);
        void close_all_connections();
        [[nodiscard]] std::size_t active_connection_count() const;
        [[nodiscard]] yuan::net::KcpServerSession::Metrics metrics() const;
        void set_connection_closed_callback(std::function<void(std::uint64_t)> callback);
        void set_handshake_validator(yuan::net::KcpServerSession::HandshakeValidator validator);

    private:
        bool write_frame(std::uint64_t connection_id, yuan::rpc::Bytes frame);

        yuan::net::NetworkRuntime &runtime_;
        yuan::net::KcpServerSession kcp_session_;
        rpc_network::RpcFrameConnectionDispatcher dispatcher_;
        std::function<void(std::uint64_t)> connection_closed_callback_;
        bool started_ = false;
    };
}

#endif
