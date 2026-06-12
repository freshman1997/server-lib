#include "global/global_process_service.h"

#include "common/tcp_rpc.h"

#include <string>
#include <utility>

namespace yuan::game::server
{
    GlobalProcessService::GlobalProcessService(GameServiceId service_id, std::uint16_t port, std::uint16_t tunnel_port)
        : port_(port),
          tunnel_port_(tunnel_port),
          service_id_(service_id),
          global_({service_id, 100, yuan::game_base::ServerRole::world, service_id.world, "global"})
    {
    }

    void GlobalProcessService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool GlobalProcessService::init()
    {
        listen_fd_ = tcp_rpc::listen_loopback(port_);
        ok_ = listen_fd_ >= 0;
        global_.set_after_echo([this](const yuan::rpc::Message &message) {
            (void)call_source_zone(message);
        });
        return ok_;
    }

    void GlobalProcessService::start()
    {
        ok_ = listen_fd_ >= 0 && register_to_tunnel() && tcp_rpc::serve_one(listen_fd_, global_.rpc_server());
    }

    void GlobalProcessService::stop()
    {
        tcp_rpc::close_fd(listen_fd_);
        listen_fd_ = -1;
    }

    bool GlobalProcessService::ok() const
    {
        return ok_;
    }

    bool GlobalProcessService::register_to_tunnel()
    {
        TunnelRegistration registration;
        registration.service_id = service_id_.pack();
        registration.port = port_;
        registration.name = "global";
        yuan::rpc::Bytes payload;
        if (!encode_tunnel_registration(registration, payload)) {
            return false;
        }
        yuan::rpc::Message message;
        message.route.name = std::string(route::tunnel_register);
        message.payload = std::move(payload);
        auto response = tcp_rpc::call(tunnel_port_, message);
        return response && response->status == yuan::rpc::RpcStatus::ok;
    }

    bool GlobalProcessService::call_source_zone(const yuan::rpc::Message &message)
    {
        const auto it = message.metadata.find("tunnel.source_service_id");
        if (it == message.metadata.end()) {
            return false;
        }
        const auto source_service_id = static_cast<PackedGameServiceId>(std::stoull(it->second));
        TunnelEnvelope envelope;
        envelope.source_service_id = service_id_.pack();
        envelope.target_service_id = source_service_id;
        envelope.source = service_id_key(service_id_);
        envelope.target = std::to_string(source_service_id);
        envelope.request_id = 2001;
        envelope.continuation_id = 9201;
        envelope.route.name = std::string(route::zone_echo);
        envelope.payload = yuan::rpc::Codec<std::string>::encode("hello-process-zone");

        yuan::rpc::Bytes payload;
        if (!encode_tunnel_envelope(envelope, payload)) {
            return false;
        }
        yuan::rpc::Message request;
        request.request_id = envelope.request_id;
        request.set_continuation_id(envelope.continuation_id);
        request.route.name = std::string(route::tunnel_forward);
        request.payload = std::move(payload);
        auto response = tcp_rpc::call(tunnel_port_, request);
        return response && response->status == yuan::rpc::RpcStatus::ok &&
               yuan::rpc::Codec<std::string>::decode(response->payload) == "hello-process-zone" &&
               response->metadata.find("zone.node") != response->metadata.end();
    }
}
