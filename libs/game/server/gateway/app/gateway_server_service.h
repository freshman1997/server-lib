#ifndef YUAN_GAME_SERVER_GATEWAY_GATEWAY_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_GATEWAY_GATEWAY_SERVER_SERVICE_H

#include "application.h"
#include "common/rpc_network.h"
#include "gateway/rpc/gateway_msg_client.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace yuan::game::server
{
    class GatewayServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        struct Metrics
        {
            std::uint64_t zone_call_attempts = 0;
            std::uint64_t zone_call_retries = 0;
            std::uint64_t zone_call_recoveries = 0;
            std::uint64_t zone_call_failures = 0;
            std::uint64_t active_connections = 0;
            std::uint64_t active_sessions = 0;
        };

        GatewayServerService(GameServiceId service_id,
                              std::string listen_host,
                              std::uint16_t port,
                              std::string public_host,
                              std::vector<std::pair<PackedGameServiceId, std::string>> zone_endpoints,
                              std::uint64_t metrics_log_interval_ms = 0,
                              rpc_network::RpcNetworkServerConfig rpc_server_config = {},
                              std::uint64_t login_token_secret = kDefaultLoginTokenSecret,
                              std::uint64_t gateway_internal_secret = kDefaultLoginTokenSecret,
                              std::uint64_t drain_timeout_ms = 3000);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;
        bool init() override;
        void start() override;
        void stop() override;

        [[nodiscard]] bool ok() const;
        [[nodiscard]] Metrics metrics() const;
        [[nodiscard]] std::string prometheus_metrics() const;

    private:
        [[nodiscard]] PackedGameServiceId select_login_zone() const;
        [[nodiscard]] std::optional<yuan::rpc::Response> forward_to_zone(PackedGameServiceId zone_service_id,
                                                                         yuan::rpc::Route route,
                                                                         GatewayForwardContext context,
                                                                         yuan::rpc::Bytes payload,
                                                                         std::uint64_t gateway_session_id) const;
        bool push_to_client(std::uint64_t gateway_session_id, yuan::rpc::Bytes payload);
        bool close_client_session(std::uint64_t gateway_session_id);
        void enqueue_client_connection_closed(std::uint64_t connection_id);
        void cleanup_loop(std::stop_token stop_token);
        void handle_client_connection_closed(std::uint64_t connection_id, int attempt);
        void handle_session_cleanup(const GatewaySessionModel::SessionRecord &session, int attempt);
        void metrics_loop(std::stop_token stop_token) const;
        yuan::rpc::Response enqueue_gateway_message(yuan::rpc::Message message);
        void gateway_handler_loop(std::stop_token stop_token);
        bool register_deferred_gateway_route(yuan::rpc::Route route);
        void drain_sessions(std::chrono::steady_clock::time_point deadline);
        void wait_for_gateway_queue(std::chrono::steady_clock::time_point deadline);
        void wait_for_zone_pending(std::chrono::steady_clock::time_point deadline) const;
        [[nodiscard]] std::optional<rpc_network::RpcEndpoint> zone_endpoint(PackedGameServiceId zone_service_id) const;
        [[nodiscard]] std::optional<yuan::rpc::Response> call_zone(PackedGameServiceId zone_service_id,
                                                                   yuan::rpc::Route route,
                                                                   yuan::rpc::Bytes payload,
                                                                   std::uint64_t gateway_session_id = 0) const;

        std::string listen_host_;
        std::uint16_t port_ = 0;
        bool ok_ = false;
        std::atomic<bool> draining_{false};
        std::uint64_t metrics_log_interval_ms_ = 0;
        std::uint64_t drain_timeout_ms_ = 3000;
        rpc_network::RpcNetworkServerConfig rpc_server_config_;
        yuan::app::RuntimeContext context_;
        GameServiceId service_id_;
        std::unordered_map<PackedGameServiceId, rpc_network::RpcEndpoint> zone_endpoints_;
        mutable std::unordered_map<PackedGameServiceId, std::unique_ptr<rpc_network::RpcNetworkPersistentAsyncClient>> zone_clients_;
        mutable std::mutex zone_clients_mutex_;
        GatewayMsgContext gateway_context_;
        yuan::rpc::Server gateway_rpc_;
        yuan::rpc::Server gateway_handler_rpc_;
        rpc_network::RpcNetworkServer rpc_server_;
        mutable std::jthread metrics_thread_;
        std::jthread gateway_handler_thread_;
        std::jthread cleanup_thread_;
        std::mutex gateway_handler_mutex_;
        std::condition_variable_any gateway_handler_cv_;
        std::deque<yuan::rpc::Message> gateway_handler_queue_;
        struct CleanupTask
        {
            std::chrono::steady_clock::time_point due_at{};
            std::uint64_t connection_id = 0;
            GatewaySessionModel::SessionRecord session;
            int attempt = 0;
        };
        std::mutex cleanup_mutex_;
        std::condition_variable_any cleanup_cv_;
        std::vector<CleanupTask> cleanup_queue_;
        mutable std::atomic<std::uint64_t> zone_call_attempts_{0};
        mutable std::atomic<std::uint64_t> zone_call_retries_{0};
        mutable std::atomic<std::uint64_t> zone_call_recoveries_{0};
        mutable std::atomic<std::uint64_t> zone_call_failures_{0};
    };
}

#endif
