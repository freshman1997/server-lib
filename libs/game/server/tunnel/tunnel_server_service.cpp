#include "tunnel/tunnel_server_service.h"

#include <string>

namespace yuan::game::server
{
    TunnelServerService::TunnelServerService(GameServiceId service_id, std::uint16_t listen_port, std::size_t expected_requests)
        : listen_port_(listen_port),
          expected_requests_(expected_requests),
          tunnel_({service_id, 1, yuan::game_base::ServerRole::gateway, service_id.world, "tunnel"})
    {
    }

    void TunnelServerService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool TunnelServerService::init()
    {
        (void)tunnel_.rpc_server().register_handler(game_route::tunnel_register(), [this](const yuan::rpc::Message &message) {
            auto registration = decode_tunnel_registration(message.payload);
            yuan::rpc::Response response;
            if (!registration || registration->service_id == 0) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid tunnel registration";
                return response;
            }
            if (registration->port != 0) {
                const auto service_id = registration->service_id;
                const auto port = registration->port;
                if (!tunnel_.register_endpoint_handler(service_id, [port](yuan::rpc::Message forwarded) {
                        auto endpoint_response = rpc_network::RpcNetworkClient().call(rpc_network::RpcEndpoint{"127.0.0.1", port}, forwarded);
                        if (endpoint_response) {
                            return *endpoint_response;
                        }
                        yuan::rpc::Response error;
                        error.status = yuan::rpc::RpcStatus::unavailable;
                        error.error = "registered endpoint unavailable";
                        return error;
                    })) {
                    response.status = yuan::rpc::RpcStatus::internal_error;
                    response.error = "failed to register endpoint";
                    return response;
                }
            }
            response.status = yuan::rpc::RpcStatus::ok;
            return response;
        });

        ok_ = rpc_server_.bind_loopback(listen_port_, tunnel_.rpc_server(), expected_requests_);
        return ok_;
    }

    void TunnelServerService::start()
    {
        ok_ = ok_ && rpc_server_.run();
    }

    void TunnelServerService::stop()
    {
        rpc_server_.stop();
    }

    bool TunnelServerService::ok() const
    {
        return ok_;
    }
}
