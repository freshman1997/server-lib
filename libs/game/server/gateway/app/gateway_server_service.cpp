#include "gateway/app/gateway_server_service.h"

#include "logger.h"

#include <chrono>
#include <sstream>
#include <thread>

namespace yuan::game::server
{
    GatewayServerService::GatewayServerService(GameServiceId service_id,
                                                  std::string listen_host,
                                                   std::uint16_t port,
                                                   std::string public_host,
                                                   std::vector<std::pair<PackedGameServiceId, std::string>> zone_endpoints,
                                                   std::uint64_t metrics_log_interval_ms,
                                                   ClientFrameValidationOptions frame_validation_options,
                                                   rpc_network::RpcNetworkServerConfig rpc_server_config)
        : listen_host_(std::move(listen_host)),
          port_(port),
          metrics_log_interval_ms_(metrics_log_interval_ms),
          frame_validation_options_(frame_validation_options),
          rpc_server_config_(std::move(rpc_server_config)),
          service_id_(service_id),
          gateway_context_({ServiceAddress{service_id, 500, yuan::game_base::ServerRole::gateway, service_id.world, "gateway"}, std::move(public_host), port_})
    {
        for (auto &[service_id, endpoint] : zone_endpoints) {
            const auto colon = endpoint.rfind(':');
            if (service_id == 0 || colon == std::string::npos || colon + 1 >= endpoint.size()) {
                continue;
            }
            zone_endpoints_[service_id] = rpc_network::RpcEndpoint{endpoint.substr(0, colon), static_cast<std::uint16_t>(std::stoul(endpoint.substr(colon + 1)))};
        }
    }

    void GatewayServerService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool GatewayServerService::init()
    {
        if (zone_endpoints_.empty()) {
            return false;
        }
        gateway_context_.login_handler = [this](ClientLoginRequest request) {
            return login(request);
        };
        gateway_context_.game_forward_handler = [this](ClientGameRequest request, PackedGameServiceId zone_service_id) {
            return forward_game(std::move(request), zone_service_id);
        };
        gateway_context_.logout_handler = [this](ClientLoginRequest request, PackedGameServiceId zone_service_id) {
            return logout(request, zone_service_id);
        };
        gateway_context_.push_handler = [this](ClientPushMessage push) {
            return push_to_client(std::move(push));
        };
        gateway_context_.frame_validation_options = frame_validation_options_;
        if (!register_gateway_msg_client(gateway_rpc_, gateway_context_)) {
            return false;
        }
        rpc_server_.set_connection_closed_callback([this](std::uint64_t connection_id) {
            enqueue_client_connection_closed(connection_id);
        });
        rpc_server_config_.host = listen_host_;
        rpc_server_config_.port = port_;
        ok_ = rpc_server_.start(rpc_server_config_, gateway_rpc_);
        return ok_;
    }

    void GatewayServerService::start()
    {
        draining_.store(false, std::memory_order_relaxed);
        if (metrics_log_interval_ms_ != 0) {
            metrics_thread_ = std::jthread([this](std::stop_token stop_token) {
                metrics_loop(stop_token);
            });
        }
        cleanup_thread_ = std::jthread([this](std::stop_token stop_token) {
            cleanup_loop(stop_token);
        });
        ok_ = ok_ && rpc_server_.run();
    }

    void GatewayServerService::stop()
    {
        draining_.store(true, std::memory_order_relaxed);
        metrics_thread_.request_stop();
        if (metrics_thread_.joinable()) {
            metrics_thread_.join();
        }
        rpc_server_.close_all_connections();
        cleanup_thread_.request_stop();
        cleanup_cv_.notify_all();
        if (cleanup_thread_.joinable()) {
            cleanup_thread_.join();
        }
        const auto current_metrics = metrics();
        LOG_INFO("gateway zone metrics attempts={} retries={} recoveries={} failures={} active_connections={} active_sessions={}",
                 current_metrics.zone_call_attempts,
                 current_metrics.zone_call_retries,
                 current_metrics.zone_call_recoveries,
                 current_metrics.zone_call_failures,
                 current_metrics.active_connections,
                 current_metrics.active_sessions);
        rpc_server_.stop();
    }

    bool GatewayServerService::ok() const
    {
        return ok_;
    }

    GatewayServerService::Metrics GatewayServerService::metrics() const
    {
        return Metrics{zone_call_attempts_.load(std::memory_order_relaxed),
                       zone_call_retries_.load(std::memory_order_relaxed),
                       zone_call_recoveries_.load(std::memory_order_relaxed),
                       zone_call_failures_.load(std::memory_order_relaxed),
                       static_cast<std::uint64_t>(rpc_server_.active_connection_count()),
                       static_cast<std::uint64_t>(gateway_context_.sessions.session_count())};
    }

    std::string GatewayServerService::prometheus_metrics() const
    {
        const auto current = metrics();
        std::ostringstream out;
        out << "# TYPE game_gateway_zone_call_attempts counter\n"
            << "game_gateway_zone_call_attempts " << current.zone_call_attempts << "\n"
            << "# TYPE game_gateway_zone_call_retries counter\n"
            << "game_gateway_zone_call_retries " << current.zone_call_retries << "\n"
            << "# TYPE game_gateway_zone_call_recoveries counter\n"
            << "game_gateway_zone_call_recoveries " << current.zone_call_recoveries << "\n"
            << "# TYPE game_gateway_zone_call_failures counter\n"
            << "game_gateway_zone_call_failures " << current.zone_call_failures << "\n"
            << "# TYPE game_gateway_active_connections gauge\n"
            << "game_gateway_active_connections " << current.active_connections << "\n"
            << "# TYPE game_gateway_active_sessions gauge\n"
            << "game_gateway_active_sessions " << current.active_sessions << "\n";
        return out.str();
    }

    std::optional<ClientLoginResponse> GatewayServerService::login(ClientLoginRequest request) const
    {
        if (draining_.load(std::memory_order_relaxed)) {
            return ClientLoginResponse{false, request.role_id, request.zone_service_id, 0, "gateway is draining"};
        }
        const auto zone_service_id = request.zone_service_id;
        if (zone_service_id == 0) {
            return ClientLoginResponse{false, request.role_id, 0, 0, "missing target zone"};
        }

        yuan::rpc::Bytes login_payload;
        if (!encode_client_login_request(request, login_payload)) {
            return std::nullopt;
        }
        auto zone_response = call_zone(zone_service_id, game_route::zone_player_enter(), std::move(login_payload));
        if (!zone_response || zone_response->status != yuan::rpc::RpcStatus::ok) {
            return ClientLoginResponse{false, request.role_id, zone_service_id, 0, "zone login failed"};
        }
        return decode_client_login_response(zone_response->payload);
    }

    std::optional<ClientGameResponse> GatewayServerService::forward_game(ClientGameRequest request, PackedGameServiceId zone_service_id) const
    {
        if (draining_.load(std::memory_order_relaxed)) {
            return std::nullopt;
        }
        auto response = call_zone(zone_service_id, game_route::zone_echo(), request.payload, request.gateway_session_id);
        if (!response || response->status != yuan::rpc::RpcStatus::ok) {
            return std::nullopt;
        }
        return ClientGameResponse{true, request.role_id, request.gateway_session_id, response->payload, "zone game ok"};
    }

    std::optional<ClientLoginResponse> GatewayServerService::logout(ClientLoginRequest request, PackedGameServiceId zone_service_id) const
    {
        yuan::rpc::Bytes logout_payload;
        if (!encode_client_login_request(request, logout_payload)) {
            return std::nullopt;
        }
        auto response = call_zone(zone_service_id, game_route::zone_player_leave(), std::move(logout_payload), request.gateway_session_id);
        if (!response || response->status != yuan::rpc::RpcStatus::ok) {
            return std::nullopt;
        }
        return decode_client_login_response(response->payload);
    }

    bool GatewayServerService::push_to_client(ClientPushMessage push)
    {
        if (draining_.load(std::memory_order_relaxed)) {
            return false;
        }
        const auto session = gateway_context_.sessions.session_info(push.gateway_session_id);
        if (!session || session->role_id != push.role_id || session->connection_id == 0) {
            return false;
        }

        yuan::rpc::Message message;
        message.kind = yuan::rpc::MessageKind::push;
        message.route = game_route::gateway_push();
        (void)encode_client_push_message(push, message.payload);
        return rpc_server_.write_message_to_connection(session->connection_id, message);
    }

    void GatewayServerService::handle_client_connection_closed(std::uint64_t connection_id)
    {
        if (connection_id == 0) {
            return;
        }
        const auto sessions = gateway_context_.sessions.sessions_for_connection(connection_id);
        for (const auto &session : sessions) {
            ClientLoginRequest logout_request;
            logout_request.role_id = session.role_id;
            logout_request.zone_service_id = session.zone_service_id;
            logout_request.gateway_session_id = session.gateway_session_id;
            bool logout_ok = false;
            for (int attempt = 0; attempt < 3; ++attempt) {
                const auto logout_result = logout(logout_request, session.zone_service_id);
                if (logout_result && logout_result->ok) {
                    logout_ok = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{20 * (attempt + 1)});
            }
            if (!logout_ok) {
                LOG_ERROR("gateway disconnect cleanup logout failed connection={} role={} session={} zone={}",
                          connection_id,
                          session.role_id,
                          session.gateway_session_id,
                          session.zone_service_id);
            }
            gateway_context_.frame_replay_guard.erase_session(session.gateway_session_id);
            gateway_context_.sessions.logout_session(session.gateway_session_id);
        }
    }

    void GatewayServerService::enqueue_client_connection_closed(std::uint64_t connection_id)
    {
        if (connection_id == 0) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(cleanup_mutex_);
            cleanup_queue_.push_back(connection_id);
        }
        cleanup_cv_.notify_one();
    }

    void GatewayServerService::cleanup_loop(std::stop_token stop_token)
    {
        while (!stop_token.stop_requested()) {
            std::vector<std::uint64_t> pending;
            {
                std::unique_lock<std::mutex> lock(cleanup_mutex_);
                cleanup_cv_.wait(lock, stop_token, [this] {
                    return !cleanup_queue_.empty();
                });
                pending.swap(cleanup_queue_);
            }
            for (const auto connection_id : pending) {
                handle_client_connection_closed(connection_id);
            }
        }
    }

    void GatewayServerService::metrics_loop(std::stop_token stop_token) const
    {
        const auto interval = std::chrono::milliseconds(metrics_log_interval_ms_);
        while (!stop_token.stop_requested()) {
            std::this_thread::sleep_for(interval);
            if (stop_token.stop_requested()) {
                break;
            }
            const auto current_metrics = metrics();
            LOG_INFO("gateway zone metrics attempts={} retries={} recoveries={} failures={} active_connections={} active_sessions={}",
                     current_metrics.zone_call_attempts,
                     current_metrics.zone_call_retries,
                     current_metrics.zone_call_recoveries,
                     current_metrics.zone_call_failures,
                     current_metrics.active_connections,
                     current_metrics.active_sessions);
        }
    }

    std::optional<rpc_network::RpcEndpoint> GatewayServerService::zone_endpoint(PackedGameServiceId zone_service_id) const
    {
        const auto it = zone_endpoints_.find(zone_service_id);
        if (it == zone_endpoints_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<yuan::rpc::Response> GatewayServerService::call_zone(PackedGameServiceId zone_service_id,
                                                                       yuan::rpc::Route route,
                                                                       yuan::rpc::Bytes payload,
                                                                       std::uint64_t gateway_session_id) const
    {
        const auto endpoint = zone_endpoint(zone_service_id);
        if (!endpoint) {
            return std::nullopt;
        }
        yuan::rpc::Message message;
        message.kind = yuan::rpc::MessageKind::request;
        message.route = std::move(route);
        message.payload = std::move(payload);
        message.metadata["gateway.session_id"] = std::to_string(gateway_session_id);
        std::optional<yuan::rpc::Response> last_response;
        for (int attempt = 0; attempt < 3; ++attempt) {
            zone_call_attempts_.fetch_add(1, std::memory_order_relaxed);
            last_response = rpc_network::RpcNetworkClient().call(*endpoint, message);
            if (last_response && last_response->status != yuan::rpc::RpcStatus::unavailable) {
                if (attempt > 0) {
                    zone_call_recoveries_.fetch_add(1, std::memory_order_relaxed);
                    LOG_ERROR("gateway zone request recovered after retry zone_service={} route_service={} route_method={} attempts={}",
                              zone_service_id,
                              message.route.service,
                              message.route.method,
                              attempt + 1);
                }
                return last_response;
            }
            LOG_ERROR("gateway zone request failed zone_service={} route_service={} route_method={} attempt={}",
                      zone_service_id,
                      message.route.service,
                      message.route.method,
                      attempt + 1);
            if (attempt < 2) {
                zone_call_retries_.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds{20 * (attempt + 1)});
            }
        }
        zone_call_failures_.fetch_add(1, std::memory_order_relaxed);
        return last_response;
    }
}
