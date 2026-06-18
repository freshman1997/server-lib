#ifndef YUAN_GAME_SERVER_TUNNEL_TUNNEL_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_TUNNEL_TUNNEL_SERVER_SERVICE_H

#include "application.h"
#include "common/rpc_network.h"
#include "tunnel/rpc/tunnel_service.h"

#include <cstdint>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace yuan::game::server
{
    class TunnelServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        TunnelServerService(GameServiceId service_id, std::string listen_host, std::uint16_t listen_port);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;

        bool init() override;

        void start() override;

        void stop() override;

        [[nodiscard]] bool ok() const;

    private:
        yuan::rpc::Response enqueue_tunnel_message(yuan::rpc::Message message);
        void tunnel_handler_loop(std::stop_token stop_token);
        bool register_deferred_route(yuan::rpc::Route route);

        std::string listen_host_;
        std::uint16_t listen_port_ = 0;
        bool ok_ = false;
        yuan::app::RuntimeContext context_;
        TunnelService tunnel_;
        yuan::rpc::Server exposed_rpc_;
        rpc_network::RpcNetworkServer rpc_server_;
        std::jthread tunnel_handler_thread_;
        std::mutex handler_mutex_;
        std::condition_variable_any handler_cv_;
        std::deque<yuan::rpc::Message> handler_queue_;
    };
}

#endif
