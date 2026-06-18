#include "rank/app/rank_server_service.h"

#include "logger.h"
#include "option.h"

namespace yuan::game::server
{
    RankServerService::RankServerService(ServiceServerConfig config)
        : listen_host_(std::move(config.listen_host)),
          port_(config.listen_port),
          service_id_(config.service_id),
          redis_host_(std::move(config.redis_host)),
          redis_port_(config.redis_port),
          redis_db_(config.redis_db),
          redis_username_(std::move(config.redis_username)),
          redis_password_(std::move(config.redis_password)),
          redis_connect_timeout_ms_(config.redis_connect_timeout_ms),
          redis_command_timeout_ms_(config.redis_command_timeout_ms),
          tunnel_heartbeat_interval_ms_(config.tunnel_heartbeat_interval_ms),
          rank_context_(ServiceAddress{service_id_, 600, yuan::game_base::ServerRole::world, service_id_.world, "rank"})
    {
        tunnel_client_manager_.set_tunnel_endpoints(config.tunnel_endpoints);
        tunnel_client_manager_.set_heartbeat_interval_ms(tunnel_heartbeat_interval_ms_);
        tunnel_client_manager_.configure_registered_service(TunnelRegistration{service_id_.pack(), listen_host_, port_, "rank"}, "rank");
    }

    void RankServerService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool RankServerService::init()
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
        option.name_ = "game-rank";
        redis_ = std::make_shared<yuan::redis::RedisClient>(option);
        rank_context_.redis = redis_;
        if (!register_rank_msg(rank_rpc_, rank_context_)) {
            return false;
        }
        ok_ = rpc_server_.start(rpc_network::RpcNetworkServerConfig{listen_host_, port_, 0}, rank_rpc_);
        return ok_;
    }

    void RankServerService::start()
    {
        tunnel_client_manager_.start_registered_service(rpc_server_);
        ok_ = ok_ && rpc_server_.run();
    }

    void RankServerService::stop()
    {
        tunnel_client_manager_.stop_registered_service();
        rpc_server_.stop();
    }

    bool RankServerService::ok() const
    {
        return ok_;
    }

}
