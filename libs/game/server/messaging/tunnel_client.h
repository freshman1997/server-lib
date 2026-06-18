#ifndef YUAN_GAME_SERVER_MESSAGING_TUNNEL_CLIENT_H
#define YUAN_GAME_SERVER_MESSAGING_TUNNEL_CLIENT_H

#include "common/rpc_network.h"

#include <cstdint>
#include <mutex>
#include <optional>

namespace yuan::game::server
{
    class TunnelClient
    {
    public:
        explicit TunnelClient(rpc_network::RpcEndpoint endpoint);

        [[nodiscard]] const rpc_network::RpcEndpoint &endpoint() const;
        [[nodiscard]] bool alive() const;
        [[nodiscard]] std::optional<yuan::rpc::Response> send(const yuan::rpc::Message &message) const;
        [[nodiscard]] std::optional<yuan::rpc::Response> send_and_update_health(const yuan::rpc::Message &message);
        void update_health_from_response(const std::optional<yuan::rpc::Response> &response);
        void update_health_from_heartbeat(const std::optional<yuan::rpc::Response> &response);
        bool heartbeat();

    private:
        rpc_network::RpcEndpoint endpoint_;
        mutable std::mutex mutex_;
        bool alive_ = true;
        std::uint32_t missed_heartbeats_ = 0;
    };
}

#endif
