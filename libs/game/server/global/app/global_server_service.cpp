#include "global/app/global_server_service.h"

#include "common/metadata_keys.h"
#include "logger.h"

#include <string>
#include <utility>

namespace yuan::game::server
{
    GlobalServerService::GlobalServerService(ServiceServerConfig config)
        : listen_host_(std::move(config.listen_host)),
          port_(config.listen_port),
          service_id_(config.service_id),
          echo_context_({ServiceAddress{service_id_, 100, yuan::game_base::ServerRole::world, service_id_.world, "global"}})
    {
        tunnel_client_manager_.set_tunnel_endpoints(config.tunnel_endpoints);
        tunnel_client_manager_.set_heartbeat_interval_ms(config.tunnel_heartbeat_interval_ms);
        tunnel_client_manager_.configure_registered_service(TunnelRegistration{service_id_.pack(), listen_host_, port_, "global"}, "global");
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
        tunnel_client_manager_.start_registered_service(rpc_server_);
        ok_ = ok_ && rpc_server_.run();
    }

    void GlobalServerService::stop()
    {
        tunnel_client_manager_.stop_registered_service();
        rpc_server_.stop();
    }

    bool GlobalServerService::ok() const
    {
        return ok_;
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
        auto response = tunnel_client_manager_.send_to_service(service_id_.pack(),
                                                   source_service_id,
                                                   game_route::zone_echo(),
                                                   yuan::rpc::Codec<std::string>::encode("hello-server-zone"),
                                                   std::move(metadata));
        return response && response->status == yuan::rpc::RpcStatus::ok &&
               yuan::rpc::Codec<std::string>::decode(response->payload) == "hello-server-zone" &&
               response->metadata.find(game_metadata_key::zone_node) != response->metadata.end();
    }
}
