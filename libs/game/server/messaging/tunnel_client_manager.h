#ifndef YUAN_GAME_SERVER_MESSAGING_TUNNEL_CLIENT_MANAGER_H
#define YUAN_GAME_SERVER_MESSAGING_TUNNEL_CLIENT_MANAGER_H

#include "common/rpc_network.h"
#include "common/service_config.h"
#include "messaging/tunnel_client.h"
#include "messaging/tunnel_messages.h"
#include "timer/timer_handle.h"

#include <mutex>
#include <memory>
#include <optional>
#include <random>
#include <vector>
#include <atomic>
#include <cstddef>

namespace yuan::game::server
{
    enum class TunnelSelectMode
    {
        round_robin,
        random,
        hash_by_service_id,
    };

    class TunnelClientManager
    {
    public:
        struct Metrics
        {
            std::uint64_t tunnel_call_attempts = 0;
            std::uint64_t tunnel_call_retries = 0;
            std::uint64_t tunnel_call_recoveries = 0;
            std::uint64_t tunnel_call_failures = 0;
        };

        TunnelClientManager();
        explicit TunnelClientManager(std::vector<rpc_network::RpcEndpoint> tunnel_endpoints);
        explicit TunnelClientManager(std::vector<std::uint16_t> tunnel_ports);
        ~TunnelClientManager();

        TunnelClientManager(const TunnelClientManager &) = delete;
        TunnelClientManager &operator=(const TunnelClientManager &) = delete;

        void set_tunnel_endpoints(std::vector<rpc_network::RpcEndpoint> tunnel_endpoints);
        void set_tunnel_ports(std::vector<std::uint16_t> tunnel_ports);
        void add_tunnel_endpoint(rpc_network::RpcEndpoint tunnel_endpoint);
        void add_tunnel_port(std::uint16_t tunnel_port);
        void set_heartbeat_interval_ms(std::uint64_t interval_ms);
        void apply_service_config(const ServiceServerConfig &config, std::string registration_name = {});
        void configure_registered_service(TunnelRegistration registration, std::string service_name = {});
        void start_registered_service(rpc_network::RpcNetworkServer &rpc_server);
        void start_registered_service(rpc_network::RpcNetworkServer &rpc_server, TunnelRegistration registration, std::string service_name = {});
        void stop_registered_service();
        [[nodiscard]] bool empty() const;

        [[nodiscard]] std::optional<yuan::rpc::Response> register_service(TunnelRegistration registration,
                                                                           PackedGameServiceId route_key = 0,
                                                                           TunnelSelectMode mode = TunnelSelectMode::round_robin);

        [[nodiscard]] std::optional<yuan::rpc::Response> forward(TunnelEnvelope envelope,
                                                                 TunnelSelectMode mode = TunnelSelectMode::round_robin);

        [[nodiscard]] std::optional<yuan::rpc::Response> send_to_service(PackedGameServiceId source_service_id,
                                                                         PackedGameServiceId target_service_id,
                                                                          yuan::rpc::Route route,
                                                                          yuan::rpc::Bytes payload = {},
                                                                          yuan::rpc::Metadata metadata = {},
                                                                          TunnelSelectMode mode = TunnelSelectMode::round_robin);

        [[nodiscard]] std::optional<yuan::rpc::Response> send_to_random_service(PackedGameServiceId source_service_id,
                                                                                GameServiceType target_type,
                                                                                 yuan::rpc::Route route,
                                                                                 yuan::rpc::Bytes payload = {},
                                                                                 yuan::rpc::Metadata metadata = {},
                                                                                 TunnelSelectMode mode = TunnelSelectMode::round_robin);

        [[nodiscard]] std::optional<yuan::rpc::Response> broadcast_to_service_type(PackedGameServiceId source_service_id,
                                                                                   GameServiceType target_type,
                                                                                    yuan::rpc::Route route,
                                                                                    yuan::rpc::Bytes payload = {},
                                                                                    yuan::rpc::Metadata metadata = {},
                                                                                    TunnelSelectMode mode = TunnelSelectMode::round_robin);

        [[nodiscard]] std::optional<yuan::rpc::Response> call_tunnel(yuan::rpc::Message message,
                                                                      PackedGameServiceId route_key = 0,
                                                                      TunnelSelectMode mode = TunnelSelectMode::round_robin);

        [[nodiscard]] Metrics metrics() const;

    private:
        void heartbeat_tick();
        void registered_service_tick();

        mutable std::mutex mutex_;
        std::vector<std::shared_ptr<TunnelClient>> tunnels_;
        rpc_network::RpcNetworkServer *timer_owner_ = nullptr;
        yuan::timer::TimerHandle heartbeat_timer_;
        yuan::timer::TimerHandle registered_service_timer_;
        std::optional<TunnelRegistration> configured_registration_;
        std::string configured_registration_name_;
        std::chrono::milliseconds heartbeat_interval_{5000};
        std::uint64_t next_registration_due_ms_ = 0;
        std::mt19937 random_{std::random_device{}()};
        std::size_t next_tunnel_index_ = 0;
        std::atomic<std::uint64_t> tunnel_call_attempts_{0};
        std::atomic<std::uint64_t> tunnel_call_retries_{0};
        std::atomic<std::uint64_t> tunnel_call_recoveries_{0};
        std::atomic<std::uint64_t> tunnel_call_failures_{0};
    };
}

#endif
