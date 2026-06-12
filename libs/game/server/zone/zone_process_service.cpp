#include "zone/zone_process_service.h"

#include "common/tcp_rpc.h"

#include <string>
#include <thread>
#include <utility>

namespace yuan::game::server
{
    ZoneProcessService::ZoneProcessService(GameServiceId service_id, GameServiceId global_service_id, std::uint16_t tunnel_port, std::uint16_t listen_port)
        : tunnel_port_(tunnel_port),
          listen_port_(listen_port),
          service_id_(service_id),
          global_service_id_(global_service_id),
          tunnels_(),
          zone_(ServiceAddress{service_id, 200, yuan::game_base::ServerRole::scene, service_id.world, "zone"}, tunnels_)
    {
    }

    void ZoneProcessService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool ZoneProcessService::init()
    {
        listen_fd_ = tcp_rpc::listen_loopback(listen_port_);
        ok_ = tunnel_port_ != 0 && listen_fd_ >= 0;
        return ok_;
    }

    void ZoneProcessService::start()
    {
        if (!register_to_tunnel()) {
            ok_ = false;
            return;
        }
        bool reverse_ok = false;
        std::thread reverse_listener([this, &reverse_ok] {
            reverse_ok = tcp_rpc::serve_one(listen_fd_, zone_.rpc_server());
        });

        TunnelEnvelope envelope;
        envelope.source_service_id = service_id_.pack();
        envelope.target_service_id = global_service_id_.pack();
        envelope.source = service_id_key(service_id_);
        envelope.target = service_id_key(global_service_id_);
        envelope.request_id = 1001;
        envelope.continuation_id = 9001;
        envelope.route.name = std::string(route::global_echo);
        envelope.payload = yuan::rpc::Codec<std::string>::encode("hello-process-global");
        envelope.metadata["trace_id"] = "process-smoke";

        yuan::rpc::Bytes envelope_payload;
        if (!encode_tunnel_envelope(envelope, envelope_payload)) {
            ok_ = false;
            return;
        }

        yuan::rpc::Message request;
        request.kind = yuan::rpc::MessageKind::request;
        request.request_id = 1001;
        request.set_continuation_id(9001);
        request.route.name = std::string(route::tunnel_forward);
        request.payload = std::move(envelope_payload);

        auto response = tcp_rpc::call(tunnel_port_, request);
        if (!(response && response->status == yuan::rpc::RpcStatus::ok &&
              yuan::rpc::Codec<std::string>::decode(response->payload) == "hello-process-global" &&
              response->metadata.find("global.node") != response->metadata.end() &&
              response->metadata.find("tunnel.instance") != response->metadata.end())) {
            if (reverse_listener.joinable()) {
                reverse_listener.join();
            }
            ok_ = false;
            return;
        }
        if (reverse_listener.joinable()) {
            reverse_listener.join();
        }
        ok_ = reverse_ok;
    }

    void ZoneProcessService::stop()
    {
        tcp_rpc::close_fd(listen_fd_);
        listen_fd_ = -1;
    }

    bool ZoneProcessService::ok() const
    {
        return ok_;
    }

    bool ZoneProcessService::register_to_tunnel()
    {
        TunnelRegistration registration;
        registration.service_id = service_id_.pack();
        registration.port = listen_port_;
        registration.name = "zone";
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
}
