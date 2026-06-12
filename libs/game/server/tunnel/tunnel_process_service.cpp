#include "tunnel/tunnel_process_service.h"

#include "common/tcp_rpc.h"

#include <string>

namespace yuan::game::server
{
    TunnelProcessService::TunnelProcessService(GameServiceId service_id, std::uint16_t listen_port, std::size_t expected_requests)
        : listen_port_(listen_port),
          expected_requests_(expected_requests),
          tunnel_({service_id, 1, yuan::game_base::ServerRole::gateway, service_id.world, "tunnel"})
    {
    }

    void TunnelProcessService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool TunnelProcessService::init()
    {
        yuan::rpc::Route register_route;
        register_route.name = std::string(route::tunnel_register);
        (void)tunnel_.rpc_server().register_handler(register_route, [this](const yuan::rpc::Message &message) {
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
                        auto endpoint_response = tcp_rpc::call(port, forwarded);
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

        listen_fd_ = tcp_rpc::listen_loopback(listen_port_);
        ok_ = listen_fd_ >= 0;
        return ok_;
    }

    void TunnelProcessService::start()
    {
        ok_ = listen_fd_ >= 0 && tcp_rpc::serve_n_concurrent(listen_fd_, tunnel_.rpc_server(), expected_requests_);
    }

    void TunnelProcessService::stop()
    {
        tcp_rpc::close_fd(listen_fd_);
        listen_fd_ = -1;
    }

    bool TunnelProcessService::ok() const
    {
        return ok_;
    }
}
