#ifndef YUAN_GAME_SERVER_GLOBAL_DB_PROXY_APP_GLOBAL_DB_PROXY_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_GLOBAL_DB_PROXY_APP_GLOBAL_DB_PROXY_SERVER_SERVICE_H

#include "application.h"
#include "common/rpc_network.h"
#include "common/service_config.h"
#include "global_db_proxy/rpc/global_db_msg.h"
#include "messaging/tunnel_client_manager.h"

#include "redis_client.h"
#include "redis_async_executor.h"
#include "redis_client_pool.h"

#include <memory>
#include <string>
#include <thread>

namespace yuan::game::server
{
    class GlobalDbProxyServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        explicit GlobalDbProxyServerService(ServiceServerConfig config);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;
        bool init() override;
        void start() override;
        void stop() override;
        [[nodiscard]] bool ok() const;

    private:
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
        std::uint16_t redis_pool_size_ = 4;
        std::uint64_t tunnel_heartbeat_interval_ms_ = 5000;
        std::shared_ptr<yuan::redis::RedisClient> redis_;
        std::shared_ptr<yuan::redis::RedisClientPool> redis_pool_;
        std::unique_ptr<yuan::redis::RedisAsyncExecutor> redis_executor_;
        GlobalDbMsgContext global_db_context_;
        yuan::rpc::Server global_db_rpc_;
        rpc_network::RpcNetworkServer rpc_server_;
        TunnelClientManager tunnel_client_manager_;
        bool ok_ = false;
    };
}

#endif
