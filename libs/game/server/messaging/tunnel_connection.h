#ifndef YUAN_GAME_SERVER_MESSAGING_TUNNEL_CONNECTION_H
#define YUAN_GAME_SERVER_MESSAGING_TUNNEL_CONNECTION_H

#include "common/rpc_network.h"

#include <cstdint>
#include <mutex>
#include <optional>

namespace yuan::game::server
{
    class TunnelConnection
    {
    public:
        explicit TunnelConnection(rpc_network::RpcEndpoint endpoint);

        [[nodiscard]] const rpc_network::RpcEndpoint &endpoint() const;
        [[nodiscard]] bool alive() const;
        [[nodiscard]] std::optional<yuan::rpc::Response> send(const yuan::rpc::Message &message) const;
        [[nodiscard]] std::optional<yuan::rpc::Response> send_and_update_health(const yuan::rpc::Message &message);
        bool heartbeat();

    private:
        rpc_network::RpcEndpoint endpoint_;
        mutable std::mutex mutex_;
        bool alive_ = true;
        std::uint32_t missed_heartbeats_ = 0;
    };
}

#endif
