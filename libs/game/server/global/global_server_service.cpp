#include "global/global_server_service.h"

#include <string>
#include <utility>

namespace yuan::game::server
{
    GlobalServerService::GlobalServerService(GameServiceId service_id, std::uint16_t port, std::uint16_t tunnel_port)
        : port_(port),
          tunnel_port_(tunnel_port),
          service_id_(service_id),
          global_({service_id, 100, yuan::game_base::ServerRole::world, service_id.world, "global"}),
          messaging_({tunnel_port})
    {
    }

    void GlobalServerService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool GlobalServerService::init()
    {
        ok_ = rpc_server_.bind_loopback(port_, global_.rpc_server(), 1);
        global_.set_after_echo([this](const yuan::rpc::Message &message) {
            (void)call_source_zone(message);
        });
        return ok_;
    }

    void GlobalServerService::start()
    {
        messaging_.start_heartbeat();
        ok_ = ok_ && register_to_tunnel() && rpc_server_.run();
    }

    void GlobalServerService::stop()
    {
        messaging_.stop_heartbeat();
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
        registration.port = port_;
        registration.name = "global";
        auto response = messaging_.register_service(std::move(registration));
        return response && response->status == yuan::rpc::RpcStatus::ok;
    }

    bool GlobalServerService::call_source_zone(const yuan::rpc::Message &message)
    {
        const auto it = message.metadata.find("tunnel.source_service_id");
        if (it == message.metadata.end()) {
            return false;
        }
        const auto source_service_id = static_cast<PackedGameServiceId>(std::stoull(it->second));
        yuan::rpc::Metadata metadata;
        metadata["tunnel.source"] = service_id_key(service_id_);
        auto response = messaging_.send_to_service(service_id_.pack(),
                                                   source_service_id,
                                                   game_route::zone_echo(),
                                                   yuan::rpc::Codec<std::string>::encode("hello-server-zone"),
                                                   std::move(metadata));
        return response && response->status == yuan::rpc::RpcStatus::ok &&
               yuan::rpc::Codec<std::string>::decode(response->payload) == "hello-server-zone" &&
               response->metadata.find("zone.node") != response->metadata.end();
    }
}
