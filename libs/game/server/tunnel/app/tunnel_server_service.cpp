#include "tunnel/app/tunnel_server_service.h"

#include <cstdlib>
#include <functional>
#include <string>

namespace yuan::game::server
{
    namespace
    {
        yuan::rpc::Response handle_registered_network_endpoint(std::string host, std::uint16_t port, yuan::rpc::Message forwarded)
        {
            auto endpoint_response = rpc_network::RpcNetworkClient().call(rpc_network::RpcEndpoint{std::move(host), port}, forwarded);
            if (endpoint_response) {
                return *endpoint_response;
            }
            yuan::rpc::Response error;
            error.status = yuan::rpc::RpcStatus::unavailable;
            error.error = "registered endpoint unavailable";
            return error;
        }

    }

    TunnelServerService::TunnelServerService(ServiceServerConfig config)
        : listen_host_(std::move(config.listen_host)),
          listen_port_(config.listen_port),
          tunnel_({config.service_id, 1, yuan::game_base::ServerRole::gateway, config.service_id.world, "tunnel"})
    {
    }

    void TunnelServerService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool TunnelServerService::init()
    {
        (void)tunnel_.rpc_server().register_handler(game_route::tunnel_register(), std::bind_front(&TunnelServerService::handle_tunnel_register, this));

        if (!register_deferred_route(game_route::tunnel_register()) ||
            !register_deferred_route(game_route::tunnel_forward()) ||
            !register_deferred_route(game_route::tunnel_reply()) ||
            !register_deferred_route(game_route::tunnel_heartbeat())) {
            return false;
        }
        ok_ = rpc_server_.start(rpc_network::RpcNetworkServerConfig{listen_host_, listen_port_, 0}, exposed_rpc_);
        return ok_;
    }

    void TunnelServerService::start()
    {
        tunnel_handler_thread_ = std::jthread([this](std::stop_token stop_token) {
            tunnel_handler_loop(stop_token);
        });
        ok_ = ok_ && rpc_server_.run();
    }

    void TunnelServerService::stop()
    {
        tunnel_handler_thread_.request_stop();
        handler_cv_.notify_all();
        if (tunnel_handler_thread_.joinable()) {
            tunnel_handler_thread_.join();
        }
        rpc_server_.stop();
    }

    bool TunnelServerService::ok() const
    {
        return ok_;
    }

    bool TunnelServerService::register_deferred_route(yuan::rpc::Route route)
    {
        return exposed_rpc_.register_handler(route, std::bind_front(&TunnelServerService::enqueue_tunnel_message, this));
    }

    yuan::rpc::Response TunnelServerService::handle_tunnel_register(const yuan::rpc::Message &message)
    {
        auto registration = decode_tunnel_registration(message.payload);
        yuan::rpc::Response response;
        if (!registration || registration->service_id == 0) {
            response.status = yuan::rpc::RpcStatus::bad_request;
            response.error = "invalid tunnel registration";
            return response;
        }
        
        if (registration->port != 0) {
            const auto service_id = registration->service_id;
            const auto host = registration->host;
            const auto port = registration->port;
            if (host.empty()) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "registered endpoint host is required";
                return response;
            }

            if (!tunnel_.register_endpoint_handler(service_id, std::bind_front(handle_registered_network_endpoint, host, port))) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "failed to register endpoint";
                return response;
            }
        }

        response.status = yuan::rpc::RpcStatus::ok;
        return response;
    }

    yuan::rpc::Response TunnelServerService::enqueue_tunnel_message(yuan::rpc::Message message)
    {
        yuan::rpc::Response response;
        response.request_id = message.request_id;
        response.set_continuation_id(message.continuation_id());
        if (!message.metadata.contains(rpc_network::metadata_key::connection_id)) {
            response.status = yuan::rpc::RpcStatus::bad_request;
            response.error = "missing connection id";
            return response;
        }
        {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            handler_queue_.push_back(std::move(message));
        }
        handler_cv_.notify_one();
        response.metadata[rpc_network::metadata_key::defer_response] = "1";
        return response;
    }

    void TunnelServerService::tunnel_handler_loop(std::stop_token stop_token)
    {
        while (!stop_token.stop_requested()) {
            std::deque<yuan::rpc::Message> pending;
            {
                std::unique_lock<std::mutex> lock(handler_mutex_);
                handler_cv_.wait(lock, stop_token, [this] { return !handler_queue_.empty(); });
                if (stop_token.stop_requested()) {
                    break;
                }
                pending.swap(handler_queue_);
            }
            for (auto &message : pending) {
                const auto connection_id = static_cast<std::uint64_t>(std::strtoull(message.metadata[rpc_network::metadata_key::connection_id].c_str(), nullptr, 10));
                auto response = tunnel_.rpc_server().handle(std::move(message));
                (void)rpc_server_.write_response_to_connection(connection_id, std::move(response));
            }
        }
    }
}
