#ifndef YUAN_GAME_SERVER_WORLD_DB_PROXY_APP_WORLD_DB_PROXY_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_WORLD_DB_PROXY_APP_WORLD_DB_PROXY_SERVER_SERVICE_H

#include "application.h"
#include "common/rpc_network.h"
#include "messaging/process_message_manager.h"
#include "world_db_proxy/rpc/world_db_msg.h"

#include "redis_client.h"

#include <memory>
#include <string>
#include <thread>

namespace yuan::game::server
{
    class WorldDbProxyServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        WorldDbProxyServerService(GameServiceId service_id,
                                  std::string listen_host,
                                  std::uint16_t port,
                                  std::vector<rpc_network::RpcEndpoint> tunnel_endpoints,
                                  std::string redis_host,
                                  std::uint16_t redis_port,
                                  std::uint16_t redis_db,
                                  std::string redis_username,
                                  std::string redis_password,
                                  std::uint16_t redis_connect_timeout_ms,
                                  std::uint16_t redis_command_timeout_ms,
                                  std::uint64_t tunnel_heartbeat_interval_ms);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;
        bool init() override;
        void start() override;
        void stop() override;
        [[nodiscard]] bool ok() const;

    private:
        bool register_to_tunnel();
        void register_loop(std::stop_token stop_token);

        yuan::app::RuntimeContext context_;
        GameServiceId service_id_;
        std::string listen_host_;
        std::uint16_t port_ = 0;
        std::string redis_host_;
        std::uint16_t redis_port_ = 6379;
        std::uint16_t redis_db_ = 0;
        std::string redis_username_;
        std::string redis_password_;
        std::uint16_t redis_connect_timeout_ms_ = 1000;
        std::uint16_t redis_command_timeout_ms_ = 1000;
        std::uint64_t tunnel_heartbeat_interval_ms_ = 5000;
        std::shared_ptr<yuan::redis::RedisClient> redis_;
        WorldDbMsgContext world_db_context_;
        yuan::rpc::Server world_db_rpc_;
        rpc_network::RpcNetworkServer rpc_server_;
        ProcessMessageManager messaging_;
        std::jthread register_thread_;
        bool ok_ = false;
    };
}

#endif
