#include "gateway/app/gateway_websocket_transport.h"

namespace yuan::game::server
{
    namespace
    {
        yuan::rpc::Bytes to_bytes(const yuan::buffer::ByteBuffer &buffer)
        {
            const auto span = buffer.readable_span();
            const auto *data = reinterpret_cast<const std::uint8_t *>(span.data());
            return yuan::rpc::Bytes(data, data + span.size());
        }
    }

    GatewayWebSocketTransport::GatewayWebSocketTransport(yuan::rpc::Server &server, yuan::net::NetworkRuntime &runtime)
        : runtime_(runtime), dispatcher_(server)
    {
        dispatcher_.set_callbacks(rpc_network::RpcFrameConnectionDispatcher::Callbacks{
            [this](std::uint64_t connection_id, yuan::rpc::Bytes frame) { return write_frame(connection_id, std::move(frame)); },
            [this](std::uint64_t connection_id) { (void)close_connection(connection_id); },
            [this](std::uint64_t connection_id) {
                if (connection_closed_callback_) {
                    connection_closed_callback_(connection_id);
                }
            }});
    }

    GatewayWebSocketTransport::~GatewayWebSocketTransport()
    {
        stop();
    }

    bool GatewayWebSocketTransport::start(std::uint16_t port, std::size_t max_buffered_bytes)
    {
        dispatcher_.set_max_buffered_bytes(max_buffered_bytes);
        server_.set_data_handler(this);
        started_ = server_.init(port, runtime_);
        return started_;
    }

    void GatewayWebSocketTransport::serve()
    {
        if (started_) {
            server_.serve();
        }
    }

    void GatewayWebSocketTransport::stop()
    {
        if (started_) {
            server_.stop();
            started_ = false;
        }
    }

    bool GatewayWebSocketTransport::write_message_to_connection(std::uint64_t connection_id, const yuan::rpc::Message &message)
    {
        return dispatcher_.write_message(connection_id, message);
    }

    bool GatewayWebSocketTransport::write_response_to_connection(std::uint64_t connection_id, yuan::rpc::Response response)
    {
        return dispatcher_.write_response(connection_id, std::move(response));
    }

    bool GatewayWebSocketTransport::close_connection(std::uint64_t connection_id)
    {
        yuan::net::websocket::WebSocketConnection *connection = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = connections_by_id_.find(connection_id);
            if (it == connections_by_id_.end()) {
                return false;
            }
            connection = it->second;
        }
        runtime_.dispatch([connection] {
            if (connection) {
                connection->close();
            }
        });
        return true;
    }

    void GatewayWebSocketTransport::close_all_connections()
    {
        std::vector<yuan::net::websocket::WebSocketConnection *> connections;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connections.reserve(connections_by_id_.size());
            for (const auto &[id, connection] : connections_by_id_) {
                (void)id;
                connections.push_back(connection);
            }
        }
        runtime_.dispatch([connections = std::move(connections)] {
            for (auto *connection : connections) {
                if (connection) {
                    connection->close();
                }
            }
        });
    }

    std::size_t GatewayWebSocketTransport::active_connection_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return connections_by_id_.size();
    }

    void GatewayWebSocketTransport::set_connection_closed_callback(std::function<void(std::uint64_t)> callback)
    {
        connection_closed_callback_ = std::move(callback);
    }

    void GatewayWebSocketTransport::on_connected(yuan::net::websocket::WebSocketConnection *wsConn)
    {
        if (!wsConn) {
            return;
        }
        const auto id = next_connection_id_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mutex_);
        ids_by_connection_[wsConn] = id;
        connections_by_id_[id] = wsConn;
    }

    void GatewayWebSocketTransport::on_data(yuan::net::websocket::WebSocketConnection *wsConn, const yuan::buffer::ByteBuffer &buff)
    {
        const auto connection_id = connection_id_for(wsConn);
        if (connection_id == 0) {
            if (wsConn) {
                wsConn->close();
            }
            return;
        }
        (void)dispatcher_.on_bytes(connection_id, to_bytes(buff));
    }

    void GatewayWebSocketTransport::on_close(yuan::net::websocket::WebSocketConnection *wsConn)
    {
        std::uint64_t connection_id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = ids_by_connection_.find(wsConn);
            if (it != ids_by_connection_.end()) {
                connection_id = it->second;
                ids_by_connection_.erase(it);
                connections_by_id_.erase(connection_id);
            }
        }
        if (connection_id != 0) {
            dispatcher_.erase(connection_id);
        }
    }

    std::uint64_t GatewayWebSocketTransport::connection_id_for(yuan::net::websocket::WebSocketConnection *wsConn) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = ids_by_connection_.find(wsConn);
        return it == ids_by_connection_.end() ? 0 : it->second;
    }

    bool GatewayWebSocketTransport::write_frame(std::uint64_t connection_id, yuan::rpc::Bytes frame)
    {
        yuan::net::websocket::WebSocketConnection *connection = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = connections_by_id_.find(connection_id);
            if (it == connections_by_id_.end()) {
                return false;
            }
            connection = it->second;
        }
        runtime_.dispatch([connection, frame = std::move(frame)] {
            if (connection && connection->connected()) {
                (void)connection->send(reinterpret_cast<const char *>(frame.data()), frame.size(), yuan::net::websocket::WebSocketConnection::PacketType::binary_);
            }
        });
        return true;
    }
}
