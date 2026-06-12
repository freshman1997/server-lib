#ifndef YUAN_GAME_SERVER_MESSAGING_PROCESS_MESSAGE_MANAGER_H
#define YUAN_GAME_SERVER_MESSAGING_PROCESS_MESSAGE_MANAGER_H

#include "common/rpc_network.h"
#include "messaging/tunnel_connection.h"
#include "messaging/tunnel_messages.h"

#include <mutex>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <vector>

namespace yuan::game::server
{
    enum class TunnelSelectMode
    {
        random,
        hash_by_service_id,
    };

    class ProcessMessageManager
    {
    public:
        ProcessMessageManager();
        explicit ProcessMessageManager(std::vector<std::uint16_t> tunnel_ports);
        ~ProcessMessageManager();

        ProcessMessageManager(const ProcessMessageManager &) = delete;
        ProcessMessageManager &operator=(const ProcessMessageManager &) = delete;

        void set_tunnel_ports(std::vector<std::uint16_t> tunnel_ports);
        void add_tunnel_port(std::uint16_t tunnel_port);
        void start_heartbeat();
        void stop_heartbeat();
        [[nodiscard]] bool empty() const;

        [[nodiscard]] std::optional<rpc_network::RpcEndpoint> select_tunnel(PackedGameServiceId route_key = 0,
                                                                             TunnelSelectMode mode = TunnelSelectMode::hash_by_service_id);

        [[nodiscard]] std::optional<yuan::rpc::Response> register_service(TunnelRegistration registration,
                                                                          PackedGameServiceId route_key = 0,
                                                                          TunnelSelectMode mode = TunnelSelectMode::hash_by_service_id);

        [[nodiscard]] std::optional<yuan::rpc::Response> forward(TunnelEnvelope envelope,
                                                                 TunnelSelectMode mode = TunnelSelectMode::hash_by_service_id);

        [[nodiscard]] std::optional<yuan::rpc::Response> send_to_service(PackedGameServiceId source_service_id,
                                                                         PackedGameServiceId target_service_id,
                                                                         yuan::rpc::Route route,
                                                                         yuan::rpc::Bytes payload = {},
                                                                         yuan::rpc::Metadata metadata = {},
                                                                         TunnelSelectMode mode = TunnelSelectMode::hash_by_service_id);

        [[nodiscard]] std::optional<yuan::rpc::Response> send_to_random_service(PackedGameServiceId source_service_id,
                                                                                GameServiceType target_type,
                                                                                yuan::rpc::Route route,
                                                                                yuan::rpc::Bytes payload = {},
                                                                                yuan::rpc::Metadata metadata = {},
                                                                                TunnelSelectMode mode = TunnelSelectMode::random);

        [[nodiscard]] std::optional<yuan::rpc::Response> broadcast_to_service_type(PackedGameServiceId source_service_id,
                                                                                   GameServiceType target_type,
                                                                                   yuan::rpc::Route route,
                                                                                   yuan::rpc::Bytes payload = {},
                                                                                   yuan::rpc::Metadata metadata = {},
                                                                                   TunnelSelectMode mode = TunnelSelectMode::random);

        [[nodiscard]] std::optional<yuan::rpc::Response> call_tunnel(yuan::rpc::Message message,
                                                                     PackedGameServiceId route_key = 0,
                                                                     TunnelSelectMode mode = TunnelSelectMode::hash_by_service_id);

    private:
        void heartbeat_loop(std::stop_token stop_token);

        mutable std::mutex mutex_;
        std::vector<std::shared_ptr<TunnelConnection>> tunnels_;
        std::jthread heartbeat_thread_;
        std::mt19937 random_{std::random_device{}()};
    };
}

#endif
