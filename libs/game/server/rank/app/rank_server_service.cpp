#include "rank/app/rank_server_service.h"

#include "logger.h"
#include "option.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace yuan::game::server
{
    namespace
    {
        bool wait_for_stop(std::stop_token stop_token, std::chrono::milliseconds delay)
        {
            std::mutex mutex;
            std::condition_variable_any cv;
            std::unique_lock<std::mutex> lock(mutex);
            (void)cv.wait_for(lock, stop_token, delay, [] { return false; });
            return stop_token.stop_requested();
        }
    }

    RankServerService::RankServerService(GameServiceId service_id,
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
                                         std::uint64_t tunnel_heartbeat_interval_ms)
        : listen_host_(std::move(listen_host)),
          port_(port),
          service_id_(service_id),
          redis_host_(std::move(redis_host)),
          redis_port_(redis_port),
          redis_db_(redis_db),
          redis_username_(std::move(redis_username)),
          redis_password_(std::move(redis_password)),
          redis_connect_timeout_ms_(redis_connect_timeout_ms),
          redis_command_timeout_ms_(redis_command_timeout_ms),
          tunnel_heartbeat_interval_ms_(tunnel_heartbeat_interval_ms),
          rank_context_(ServiceAddress{service_id, 600, yuan::game_base::ServerRole::world, service_id.world, "rank"}),
          messaging_(std::move(tunnel_endpoints))
    {
        messaging_.set_heartbeat_interval_ms(tunnel_heartbeat_interval_ms_);
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
        messaging_.start_heartbeat();
        register_thread_ = std::jthread([this](std::stop_token stop_token) { register_loop(stop_token); });
        ok_ = ok_ && rpc_server_.run();
    }

    void RankServerService::stop()
    {
        messaging_.stop_heartbeat();
        register_thread_.request_stop();
        if (register_thread_.joinable()) {
            register_thread_.join();
        }
        rpc_server_.stop();
    }

    bool RankServerService::ok() const
    {
        return ok_;
    }

    bool RankServerService::register_to_tunnel()
    {
        TunnelRegistration registration;
        registration.service_id = service_id_.pack();
        registration.host = listen_host_;
        registration.port = port_;
        registration.name = "rank";
        auto response = messaging_.register_service(std::move(registration));
        return response && response->status == yuan::rpc::RpcStatus::ok;
    }

    void RankServerService::register_loop(std::stop_token stop_token)
    {
        while (!stop_token.stop_requested()) {
            if (register_to_tunnel()) {
                (void)wait_for_stop(stop_token, std::chrono::seconds{5});
            } else {
                LOG_ERROR("rank failed to register to tunnel service_id={}", service_id_.pack());
                (void)wait_for_stop(stop_token, std::chrono::milliseconds{500});
            }
        }
    }
}
