#include "messaging/tunnel_client.h"
#include "common/game_rpc_protocol.h"

namespace yuan::game::server
{
    namespace
    {
        constexpr std::uint32_t max_missed_heartbeats = 3;
    }

    TunnelClient::TunnelClient(rpc_network::RpcEndpoint endpoint)
        : endpoint_(std::move(endpoint))
    {
    }

    const rpc_network::RpcEndpoint &TunnelClient::endpoint() const
    {
        return endpoint_;
    }

    bool TunnelClient::alive() const
    {
        std::scoped_lock lock(mutex_);
        return alive_;
    }

    std::optional<yuan::rpc::Response> TunnelClient::send(const yuan::rpc::Message &message) const
    {
        return rpc_network::RpcNetworkClient().call(endpoint_, message);
    }

    std::optional<yuan::rpc::Response> TunnelClient::send_and_update_health(const yuan::rpc::Message &message)
    {
        const auto response = send(message);
        update_health_from_response(response);
        return response;
    }

    void TunnelClient::update_health_from_response(const std::optional<yuan::rpc::Response> &response)
    {
        std::scoped_lock lock(mutex_);
        if (response && response->status != yuan::rpc::RpcStatus::unavailable) {
            alive_ = true;
            missed_heartbeats_ = 0;
        } else if (++missed_heartbeats_ >= max_missed_heartbeats) {
            alive_ = false;
        }
    }

    void TunnelClient::update_health_from_heartbeat(const std::optional<yuan::rpc::Response> &response)
    {
        const auto ok = response && response->status == yuan::rpc::RpcStatus::ok;
        std::scoped_lock lock(mutex_);
        if (ok) {
            missed_heartbeats_ = 0;
            alive_ = true;
        } else if (++missed_heartbeats_ >= max_missed_heartbeats) {
            alive_ = false;
        }
    }

    bool TunnelClient::heartbeat()
    {
        yuan::rpc::Message message;
        message.route = game_route::tunnel_heartbeat();
        const auto response = send(message);
        update_health_from_heartbeat(response);
        return response && response->status == yuan::rpc::RpcStatus::ok;
    }
}
