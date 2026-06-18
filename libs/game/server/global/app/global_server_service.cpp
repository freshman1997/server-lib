#include "global/app/global_server_service.h"

#include "common/metadata_keys.h"
#include "logger.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <utility>

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

    GlobalServerService::GlobalServerService(GameServiceId service_id,
                                             std::string listen_host,
                                             std::uint16_t port,
                                             std::vector<rpc_network::RpcEndpoint> tunnel_endpoints,
                                             std::uint64_t tunnel_heartbeat_interval_ms)
        : listen_host_(std::move(listen_host)),
          port_(port),
          service_id_(service_id),
          echo_context_({ServiceAddress{service_id, 100, yuan::game_base::ServerRole::world, service_id.world, "global"}}),
          messaging_(std::move(tunnel_endpoints))
    {
        messaging_.set_heartbeat_interval_ms(tunnel_heartbeat_interval_ms);
    }

    void GlobalServerService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool GlobalServerService::init()
    {
        echo_context_.after_echo = [this](const yuan::rpc::Message &message) {
            (void)call_source_zone(message);
        };
        register_global_builtin_gm(gm_context_);
        const bool registered = register_global_msg_echo(global_rpc_, echo_context_) && register_global_msg_gm(global_rpc_, gm_context_);
        if (!registered) {
            return false;
        }
        ok_ = rpc_server_.start(rpc_network::RpcNetworkServerConfig{listen_host_, port_, 0}, global_rpc_);
        return ok_;
    }

    void GlobalServerService::start()
    {
        messaging_.start_heartbeat();
        register_thread_ = std::jthread([this](std::stop_token stop_token) {
            register_loop(stop_token);
        });
        ok_ = ok_ && rpc_server_.run();
    }

    void GlobalServerService::stop()
    {
        messaging_.stop_heartbeat();
        register_thread_.request_stop();
        if (register_thread_.joinable()) {
            register_thread_.join();
        }
        rpc_server_.stop();
    }

    bool GlobalServerService::ok() const
    {
        return ok_;
    }

    bool GlobalServerService::register_to_tunnel()
    {
        TunnelRegistration registration;
        registration.service_id = service_id_.pack();
        registration.host = listen_host_;
        registration.port = port_;
        registration.name = "global";
        auto response = messaging_.register_service(std::move(registration));
        return response && response->status == yuan::rpc::RpcStatus::ok;
    }

    void GlobalServerService::register_loop(std::stop_token stop_token)
    {
        while (!stop_token.stop_requested()) {
            if (register_to_tunnel()) {
                (void)wait_for_stop(stop_token, std::chrono::seconds{5});
            } else {
                LOG_ERROR("global failed to register to tunnel service_id={}", service_id_.pack());
                (void)wait_for_stop(stop_token, std::chrono::milliseconds{500});
            }
        }
    }

    bool GlobalServerService::call_source_zone(const yuan::rpc::Message &message)
    {
        const auto it = message.metadata.find(game_metadata_key::tunnel_source_service_id);
        if (it == message.metadata.end()) {
            return false;
        }
        const auto source_service_id = static_cast<PackedGameServiceId>(std::stoull(it->second));
        yuan::rpc::Metadata metadata;
        metadata[game_metadata_key::tunnel_source] = service_id_key(service_id_);
        auto response = messaging_.send_to_service(service_id_.pack(),
                                                   source_service_id,
                                                   game_route::zone_echo(),
                                                   yuan::rpc::Codec<std::string>::encode("hello-server-zone"),
                                                   std::move(metadata));
        return response && response->status == yuan::rpc::RpcStatus::ok &&
               yuan::rpc::Codec<std::string>::decode(response->payload) == "hello-server-zone" &&
               response->metadata.find(game_metadata_key::zone_node) != response->metadata.end();
    }
}
