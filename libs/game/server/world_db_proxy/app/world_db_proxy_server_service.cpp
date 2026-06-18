#include "world_db_proxy/app/world_db_proxy_server_service.h"

#include "logger.h"
#include "option.h"

namespace yuan::game::server
{
    WorldDbProxyServerService::WorldDbProxyServerService(ServiceServerConfig config)
        : service_id_(config.service_id),
          listen_host_(std::move(config.listen_host)),
          port_(config.listen_port),
          redis_host_(std::move(config.redis_host)),
          redis_port_(config.redis_port),
          redis_db_(config.redis_db),
          redis_username_(std::move(config.redis_username)),
          redis_password_(std::move(config.redis_password)),
          redis_connect_timeout_ms_(config.redis_connect_timeout_ms),
          redis_command_timeout_ms_(config.redis_command_timeout_ms),
          redis_pool_size_(config.redis_pool_size == 0 ? 1 : config.redis_pool_size),
          tunnel_heartbeat_interval_ms_(config.tunnel_heartbeat_interval_ms),
          world_db_context_(ServiceAddress{service_id_, 710, yuan::game_base::ServerRole::world, service_id_.world, "world_db_proxy"})
    {
        tunnel_client_manager_.set_tunnel_endpoints(config.tunnel_endpoints);
        tunnel_client_manager_.set_heartbeat_interval_ms(tunnel_heartbeat_interval_ms_);
        tunnel_client_manager_.configure_registered_service(TunnelRegistration{service_id_.pack(), listen_host_, port_, "world_db_proxy"}, "world_db_proxy");
    }

    void WorldDbProxyServerService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool WorldDbProxyServerService::init()
    {
        yuan::redis::Option option;
        option.host_ = redis_host_;
        option.port_ = redis_port_;
        option.db_ = redis_db_;
        option.username_ = redis_username_;
        option.password_ = redis_password_;
        option.timeout_ms_ = redis_command_timeout_ms_;
        option.command_timeout_ms_ = redis_command_timeout_ms_;
        option.connect_timeout_ms_ = redis_connect_timeout_ms_;
        option.name_ = "game-world-db-proxy";
        redis_pool_ = std::make_shared<yuan::redis::RedisClientPool>();
        if (!redis_pool_->init(option, redis_pool_size_)) {
            return false;
        }
        redis_executor_ = std::make_unique<yuan::redis::RedisAsyncExecutor>(option);
        world_db_context_.redis_pool = redis_pool_;
        world_db_context_.redis_executor = redis_executor_.get();
        world_db_context_.resume_runtime = rpc_server_.runtime().runtime_view().raw();
        world_db_context_.write_deferred_response = [this](std::uint64_t connection_id, yuan::rpc::Response response) {
            (void)rpc_server_.write_response_to_connection(connection_id, std::move(response));
        };
        if (!register_world_db_msg(world_db_rpc_, world_db_context_)) {
            return false;
        }
        ok_ = rpc_server_.start(rpc_network::RpcNetworkServerConfig{listen_host_, port_, 0}, world_db_rpc_);
        return ok_;
    }

    void WorldDbProxyServerService::start()
    {
        tunnel_client_manager_.start_registered_service(rpc_server_);
        ok_ = ok_ && rpc_server_.run();
    }

    void WorldDbProxyServerService::stop()
    {
        tunnel_client_manager_.stop_registered_service();
        if (redis_pool_) {
            redis_pool_->close();
        }
        if (redis_executor_) {
            redis_executor_->close();
        }
        rpc_server_.stop();
    }

    bool WorldDbProxyServerService::ok() const
    {
        return ok_;
    }

}
