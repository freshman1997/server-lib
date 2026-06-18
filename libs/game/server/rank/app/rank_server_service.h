#ifndef YUAN_GAME_SERVER_RANK_APP_RANK_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_RANK_APP_RANK_SERVER_SERVICE_H

#include "application.h"
#include "common/rpc_network.h"
#include "common/service_config.h"
#include "messaging/tunnel_client_manager.h"
#include "rank/rpc/rank_msg.h"

#include "redis_client.h"

#include <condition_variable>
#include <memory>
#include <string>
#include <thread>

namespace yuan::game::server
{
    class RankServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        explicit RankServerService(ServiceServerConfig config);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;
        bool init() override;
        void start() override;
        void stop() override;
        [[nodiscard]] bool ok() const;

    private:
        yuan::app::RuntimeContext context_;
        std::string listen_host_;
        std::uint16_t port_ = 0;
        GameServiceId service_id_;
        std::string redis_host_;
        std::uint16_t redis_port_ = 6379;
        std::uint16_t redis_db_ = 0;
        std::string redis_username_;
        std::string redis_password_;
        std::uint16_t redis_connect_timeout_ms_ = 1000;
        std::uint16_t redis_command_timeout_ms_ = 1000;
        std::uint64_t tunnel_heartbeat_interval_ms_ = 5000;
        std::shared_ptr<yuan::redis::RedisClient> redis_;
        RankMsgContext rank_context_;
        yuan::rpc::Server rank_rpc_;
        rpc_network::RpcNetworkServer rpc_server_;
        TunnelClientManager tunnel_client_manager_;
        bool ok_ = false;
    };
}

#endif
