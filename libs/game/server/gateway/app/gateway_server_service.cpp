#include "gateway/app/gateway_server_service.h"

#include "common/metadata_keys.h"
#include "logger.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
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
                                                     rpc_network::RpcNetworkServerConfig rpc_server_config,
                                                     std::uint64_t login_token_secret,
                                                     std::uint64_t gateway_internal_secret,
                                                     std::uint64_t drain_timeout_ms)
        : listen_host_(std::move(listen_host)),
          port_(port),
          metrics_log_interval_ms_(metrics_log_interval_ms),
          drain_timeout_ms_(drain_timeout_ms == 0 ? 3000 : drain_timeout_ms),
          rpc_server_config_(std::move(rpc_server_config)),
          service_id_(service_id)
    {
        gateway_context_.address = ServiceAddress{service_id, 500, yuan::game_base::ServerRole::gateway, service_id.world, "gateway"};
        gateway_context_.public_host = std::move(public_host);
        gateway_context_.public_port = port_;
        gateway_context_.login_token_secret = login_token_secret == 0 ? kDefaultLoginTokenSecret : login_token_secret;
        gateway_context_.gateway_internal_secret = gateway_internal_secret == 0 ? kDefaultLoginTokenSecret : gateway_internal_secret;
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

        if (!register_gateway_msg_client(gateway_handler_rpc_,gateway_context_,
            [this]() 
            { 
                return select_login_zone();
            },

            [this](PackedGameServiceId zone_service_id, yuan::rpc::Route route, GatewayForwardContext context, yuan::rpc::Bytes payload) 
            {
                return forward_to_zone(zone_service_id, std::move(route), context, std::move(payload), context.gateway_session_id);
            },

            [this](std::uint64_t gateway_session_id, yuan::rpc::Bytes payload) 
            {
                return push_to_client(gateway_session_id, std::move(payload));
            },

            [this](std::uint64_t gateway_session_id)
            {
                return close_client_session(gateway_session_id);
            })) 
        {
            return false;
        }
        if (!register_deferred_gateway_route(game_route::gateway_login()) ||
            !register_deferred_gateway_route(game_route::gateway_game_forward()) ||
            !register_deferred_gateway_route(game_route::gateway_logout()) ||
            !register_deferred_gateway_route(game_route::gateway_time_sync()) ||
            !register_deferred_gateway_route(game_route::gateway_push()) ||
            !register_deferred_gateway_route(game_route::gateway_session_close())) {
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

        gateway_handler_thread_ = std::jthread([this](std::stop_token stop_token) {
            gateway_handler_loop(stop_token);
        });

        cleanup_thread_ = std::jthread([this](std::stop_token stop_token) {
            cleanup_loop(stop_token);
        });

        ok_ = ok_ && rpc_server_.run();
    }

    void GatewayServerService::stop()
    {
        draining_.store(true, std::memory_order_relaxed);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(drain_timeout_ms_);
        wait_for_gateway_queue(deadline);
        drain_sessions(deadline);
        wait_for_zone_pending(deadline);
        metrics_thread_.request_stop();
        gateway_handler_thread_.request_stop();
        gateway_handler_cv_.notify_all();
        if (metrics_thread_.joinable()) {
            metrics_thread_.join();
        }
        if (gateway_handler_thread_.joinable()) {
            gateway_handler_thread_.join();
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

    PackedGameServiceId GatewayServerService::select_login_zone() const
    {
        if (draining_.load(std::memory_order_relaxed)) {
            return 0;
        }

        return zone_endpoints_.empty() ? 0 : zone_endpoints_.begin()->first;
    }

    std::optional<yuan::rpc::Response> GatewayServerService::forward_to_zone(PackedGameServiceId zone_service_id,
                                                                             yuan::rpc::Route route,
                                                                             GatewayForwardContext context,
                                                                             yuan::rpc::Bytes payload,
                                                                             std::uint64_t gateway_session_id) const
    {
        if (draining_.load(std::memory_order_relaxed)) {
            return std::nullopt;
        }

        auto response = call_zone(zone_service_id, std::move(route), std::move(payload), gateway_session_id);
        if (!response || response->status != yuan::rpc::RpcStatus::ok) {
            return std::nullopt;
        }

        return response;
    }

    bool GatewayServerService::push_to_client(std::uint64_t gateway_session_id, yuan::rpc::Bytes payload)
    {
        if (draining_.load(std::memory_order_relaxed)) {
            return false;
        }

        const auto session = gateway_context_.sessions.session_info(gateway_session_id);
        if (!session || session->connection_id == 0) {
            return false;
        }

        yuan::rpc::Message message;
        message.kind = yuan::rpc::MessageKind::push;
        message.route = game_route::gateway_push();
        message.payload = std::move(payload);
        return rpc_server_.write_message_to_connection(session->connection_id, message);
    }

    bool GatewayServerService::close_client_session(std::uint64_t gateway_session_id)
    {
        const auto session = gateway_context_.sessions.session_info(gateway_session_id);
        if (!session || session->connection_id == 0) {
            return false;
        }
        gateway_context_.sessions.logout_session(gateway_session_id);
        return rpc_server_.close_connection(session->connection_id);
    }

    void GatewayServerService::handle_client_connection_closed(std::uint64_t connection_id, int attempt)
    {
        if (connection_id == 0) {
            return;
        }

        const auto sessions = gateway_context_.sessions.sessions_for_connection(connection_id);
        for (const auto &session : sessions) {
            handle_session_cleanup(session, attempt);
        }
    }

    void GatewayServerService::handle_session_cleanup(const GatewaySessionModel::SessionRecord &session, int attempt)
    {
        yuan::rpc::Bytes logout_payload;
        const auto logout_result = forward_to_zone(session.zone_service_id,
                                                   game_route::zone_player_leave(),
                                                   GatewayForwardContext{session.gateway_session_id, session.connection_id},
                                                   logout_payload,
                                                   session.gateway_session_id);
        if (logout_result && logout_result->status == yuan::rpc::RpcStatus::ok) {
            gateway_context_.sessions.logout_session(session.gateway_session_id);
            return;
        }

        if (attempt >= 2) {
            LOG_ERROR("gateway disconnect cleanup logout failed connection={} role={} session={} zone={}",
                      session.connection_id,
                      0,
                      session.gateway_session_id,
                      session.zone_service_id);
            gateway_context_.sessions.logout_session(session.gateway_session_id);
            return;
        }

        CleanupTask retry;
        retry.due_at = std::chrono::steady_clock::now() + std::chrono::milliseconds{20 * (attempt + 1)};
        retry.session = session;
        retry.attempt = attempt + 1;
        {
            std::lock_guard<std::mutex> lock(cleanup_mutex_);
            cleanup_queue_.push_back(retry);
        }
        cleanup_cv_.notify_one();
    }

    void GatewayServerService::enqueue_client_connection_closed(std::uint64_t connection_id)
    {
        if (connection_id == 0) {
            return;
        }
        CleanupTask task;
        task.due_at = std::chrono::steady_clock::now();
        task.connection_id = connection_id;
        {
            std::lock_guard<std::mutex> lock(cleanup_mutex_);
            cleanup_queue_.push_back(task);
        }
        cleanup_cv_.notify_one();
    }

    void GatewayServerService::cleanup_loop(std::stop_token stop_token)
    {
        while (!stop_token.stop_requested()) {
            std::vector<CleanupTask> pending;
            {
                std::unique_lock<std::mutex> lock(cleanup_mutex_);
                cleanup_cv_.wait(lock, stop_token, [this] { return !cleanup_queue_.empty(); });
                if (stop_token.stop_requested()) {
                    break;
                }

                while (!cleanup_queue_.empty()) {
                    const auto next = std::min_element(cleanup_queue_.begin(), cleanup_queue_.end(), [](const CleanupTask &lhs, const CleanupTask &rhs) {
                        return lhs.due_at < rhs.due_at;
                    });

                    const auto now = std::chrono::steady_clock::now();
                    if (next->due_at > now) {
                        cleanup_cv_.wait_until(lock, stop_token, next->due_at, [this, due_at = next->due_at] {
                            return std::any_of(cleanup_queue_.begin(), cleanup_queue_.end(), [due_at](const CleanupTask &task) {
                                return task.due_at < due_at;
                            });
                        });
                        if (stop_token.stop_requested()) {
                            break;
                        }
                        continue;
                    }

                    pending.push_back(*next);
                    cleanup_queue_.erase(next);
                }

                if (stop_token.stop_requested()) {
                    break;
                }
            }

            for (const auto &task : pending) {
                if (task.connection_id != 0) {
                    handle_client_connection_closed(task.connection_id, task.attempt);
                } else if (task.session.gateway_session_id != 0) {
                    handle_session_cleanup(task.session, task.attempt);
                }
            }
        }
    }

    void GatewayServerService::metrics_loop(std::stop_token stop_token) const
    {
        const auto interval = std::chrono::milliseconds(metrics_log_interval_ms_);
        while (!stop_token.stop_requested()) {
            std::mutex wait_mutex;
            std::condition_variable_any wait_cv;
            std::unique_lock<std::mutex> lock(wait_mutex);
            if (wait_cv.wait_for(lock, stop_token, interval, [] { return false; })) {
                continue;
            }
            
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

    bool GatewayServerService::register_deferred_gateway_route(yuan::rpc::Route route)
    {
        return gateway_rpc_.register_handler(route, [this](const yuan::rpc::Message &message) {
            return enqueue_gateway_message(message);
        });
    }

    yuan::rpc::Response GatewayServerService::enqueue_gateway_message(yuan::rpc::Message message)
    {
        yuan::rpc::Response response;
        response.request_id = message.request_id;
        response.set_continuation_id(message.continuation_id());
        const auto connection_id = message.metadata.find(rpc_network::metadata_key::connection_id);
        if (connection_id == message.metadata.end()) {
            response.status = yuan::rpc::RpcStatus::bad_request;
            response.error = "missing connection id";
            return response;
        }
        if (draining_.load(std::memory_order_relaxed)) {
            response.status = yuan::rpc::RpcStatus::unavailable;
            response.error = "gateway is draining";
            response.metadata[rpc_network::metadata_key::close_connection] = "1";
            return response;
        }
        {
            std::lock_guard<std::mutex> lock(gateway_handler_mutex_);
            gateway_handler_queue_.push_back(std::move(message));
        }
        gateway_handler_cv_.notify_one();
        response.metadata[rpc_network::metadata_key::defer_response] = "1";
        return response;
    }

    void GatewayServerService::gateway_handler_loop(std::stop_token stop_token)
    {
        while (!stop_token.stop_requested()) {
            std::deque<yuan::rpc::Message> pending;
            {
                std::unique_lock<std::mutex> lock(gateway_handler_mutex_);
                gateway_handler_cv_.wait(lock, stop_token, [this] { return !gateway_handler_queue_.empty(); });
                if (stop_token.stop_requested()) {
                    break;
                }
                pending.swap(gateway_handler_queue_);
            }
            for (auto &message : pending) {
                const auto connection_id = static_cast<std::uint64_t>(std::strtoull(message.metadata[rpc_network::metadata_key::connection_id].c_str(), nullptr, 10));
                auto response = gateway_handler_rpc_.handle(std::move(message));
                (void)rpc_server_.write_response_to_connection(connection_id, std::move(response));
            }
        }
    }

    void GatewayServerService::wait_for_gateway_queue(std::chrono::steady_clock::time_point deadline)
    {
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(gateway_handler_mutex_);
                if (gateway_handler_queue_.empty()) {
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void GatewayServerService::drain_sessions(std::chrono::steady_clock::time_point deadline)
    {
        const auto sessions = gateway_context_.sessions.all_sessions();
        for (const auto &session : sessions) {
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            yuan::rpc::Bytes logout_payload;
            const auto response = call_zone(session.zone_service_id,
                                            game_route::zone_player_leave(),
                                            logout_payload,
                                            session.gateway_session_id);
            if (response && response->status == yuan::rpc::RpcStatus::ok) {
                gateway_context_.sessions.logout_session(session.gateway_session_id);
            } else {
                LOG_ERROR("gateway drain logout failed connection={} session={} zone={}",
                          session.connection_id,
                          session.gateway_session_id,
                          session.zone_service_id);
            }
        }
    }

    void GatewayServerService::wait_for_zone_pending(std::chrono::steady_clock::time_point deadline) const
    {
        while (std::chrono::steady_clock::now() < deadline) {
            bool empty = true;
            {
                std::lock_guard<std::mutex> lock(zone_clients_mutex_);
                for (const auto &[zone_service_id, client] : zone_clients_) {
                    if (client && (client->queued_size() != 0 || client->pending_size() != 0)) {
                        empty = false;
                        break;
                    }
                }
            }
            if (empty) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
        message.metadata[game_metadata_key::gateway_session_id] = std::to_string(gateway_session_id);
        std::optional<yuan::rpc::Response> last_response;
        for (int attempt = 0; attempt < 3; ++attempt) {
            zone_call_attempts_.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(zone_clients_mutex_);
                auto &client = zone_clients_[zone_service_id];
                if (!client) {
                    client = std::make_unique<rpc_network::RpcNetworkPersistentAsyncClient>();
                }
                last_response = client->call(*endpoint, message);
            }
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
            }
        }
        zone_call_failures_.fetch_add(1, std::memory_order_relaxed);
        return last_response;
    }
}
