#include "gateway/app/gateway_kcp_transport.h"

namespace yuan::game::server
{
    GatewayKcpTransport::GatewayKcpTransport(yuan::rpc::Server &server, yuan::net::NetworkRuntime &runtime)
        : runtime_(runtime), kcp_session_(runtime), dispatcher_(server)
    {
        dispatcher_.set_callbacks(rpc_network::RpcFrameConnectionDispatcher::Callbacks{
            [this](std::uint64_t connection_id, yuan::rpc::Bytes frame) { return write_frame(connection_id, std::move(frame)); },
            [this](std::uint64_t connection_id) { (void)close_connection(connection_id); },
            [this](std::uint64_t connection_id) {
                if (connection_closed_callback_) {
                    connection_closed_callback_(connection_id);
                }
            }});
        kcp_session_.set_data_callback([this](std::uint64_t connection_id, std::vector<std::uint8_t> payload) {
            (void)dispatcher_.on_bytes(connection_id, std::move(payload));
        });
        kcp_session_.set_close_callback([this](std::uint64_t connection_id) {
            dispatcher_.erase(connection_id);
        });
    }

    GatewayKcpTransport::~GatewayKcpTransport()
    {
        stop();
    }

    bool GatewayKcpTransport::start(yuan::net::KcpServerSession::Config config, std::size_t max_buffered_bytes)
    {
        dispatcher_.set_max_buffered_bytes(max_buffered_bytes);
        config.handshake_packet_type = kHandshakePacket;
        config.handshake_ack_packet_type = kHandshakeAckPacket;
        config.kcp_packet_type = kKcpPacket;
        config.first_connection_id = 20'000'000;
        started_ = kcp_session_.start(std::move(config));
        return started_;
    }

    void GatewayKcpTransport::stop()
    {
        if (!started_) {
            return;
        }
        kcp_session_.stop();
        started_ = false;
    }

    bool GatewayKcpTransport::write_message_to_connection(std::uint64_t connection_id, const yuan::rpc::Message &message)
    {
        return dispatcher_.write_message(connection_id, message);
    }

    bool GatewayKcpTransport::write_response_to_connection(std::uint64_t connection_id, yuan::rpc::Response response)
    {
        return dispatcher_.write_response(connection_id, std::move(response));
    }

    bool GatewayKcpTransport::close_connection(std::uint64_t connection_id)
    {
        return kcp_session_.close(connection_id);
    }

    void GatewayKcpTransport::close_all_connections()
    {
        kcp_session_.close_all();
    }

    std::size_t GatewayKcpTransport::active_connection_count() const
    {
        return kcp_session_.active_connection_count();
    }

    yuan::net::KcpServerSession::Metrics GatewayKcpTransport::metrics() const
    {
        return kcp_session_.metrics();
    }

    void GatewayKcpTransport::set_connection_closed_callback(std::function<void(std::uint64_t)> callback)
    {
        connection_closed_callback_ = std::move(callback);
    }

    void GatewayKcpTransport::set_handshake_validator(yuan::net::KcpServerSession::HandshakeValidator validator)
    {
        kcp_session_.set_handshake_validator(std::move(validator));
    }

    bool GatewayKcpTransport::write_frame(std::uint64_t connection_id, yuan::rpc::Bytes frame)
    {
        return kcp_session_.send(connection_id, frame);
    }
}
