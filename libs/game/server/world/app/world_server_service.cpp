#include "world/app/world_server_service.h"

#include "common/gm_command_registry.h"
#include "common/proto/world_db_proto.h"
#include "content_type.h"
#include "header_key.h"
#include "http_headers.h"
#include "middleware.h"
#include "internal/def.h"
#include "logger.h"
#include "option.h"
#include "redis_client.h"
#include "request.h"
#include "response.h"
#include "world/model/world_ownership_store.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

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

        std::string encode_http_error_json(std::string_view error)
        {
            nlohmann::json root;
            root["ok"] = false;
            root["error"] = error;
            return root.dump();
        }
    }

    WorldServerService::WorldServerService(GameServiceId service_id,
                                            std::string listen_host,
                                            std::uint16_t port,
                                            std::uint16_t http_port,
                                            std::vector<rpc_network::RpcEndpoint> tunnel_endpoints,
                                           std::string redis_host,
                                           std::uint16_t redis_port,
                                           std::uint16_t redis_db,
                                           std::string redis_username,
                                             std::string redis_password,
                                              std::uint16_t redis_connect_timeout_ms,
                                              std::uint16_t redis_command_timeout_ms,
                                              std::uint16_t redis_flush_interval_ms,
                                              std::string world_ownership_store,
                                               std::uint64_t login_reservation_ttl_ms,
                                               std::uint64_t zone_report_ttl_ms,
                                                std::uint64_t tunnel_heartbeat_interval_ms,
                                                std::uint64_t login_token_secret,
                                                WorldRoutingConfig world_routing,
                                                DbProxyRoutingConfig world_db_proxy_routing,
                                                std::uint64_t metrics_log_interval_ms)
        : listen_host_(std::move(listen_host)),
          port_(port),
          http_port_(http_port),
          service_id_(service_id),
          redis_host_(std::move(redis_host)),
          redis_port_(redis_port),
          redis_db_(redis_db),
          redis_username_(std::move(redis_username)),
          redis_password_(std::move(redis_password)),
          redis_connect_timeout_ms_(redis_connect_timeout_ms),
          redis_command_timeout_ms_(redis_command_timeout_ms),
          redis_flush_interval_ms_(redis_flush_interval_ms),
          world_ownership_store_(std::move(world_ownership_store)),
          metrics_log_interval_ms_(metrics_log_interval_ms),
          world_db_proxy_routing_(std::move(world_db_proxy_routing)),
          world_context_({ServiceAddress{service_id, 400, yuan::game_base::ServerRole::world, service_id.world, "world"}}),
          messaging_(std::move(tunnel_endpoints))
    {
        world_context_.login_reservation_ttl_ms = login_reservation_ttl_ms == 0 ? 3000 : login_reservation_ttl_ms;
        world_context_.zone_report_ttl_ms = zone_report_ttl_ms == 0 ? 3000 : zone_report_ttl_ms;
        world_context_.login_token_secret = login_token_secret == 0 ? kDefaultLoginTokenSecret : login_token_secret;
        world_context_.world_routing = std::move(world_routing);
        messaging_.set_heartbeat_interval_ms(tunnel_heartbeat_interval_ms);
    }

    void WorldServerService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool WorldServerService::init()
    {
        yuan::redis::Option option;
        option.host_ = redis_host_;
        option.port_ = redis_port_;
        option.db_ = redis_db_;
        option.username_ = redis_username_;
        option.password_ = redis_password_;
        option.timeout_ms_ = redis_command_timeout_ms_;
        option.command_timeout_ms_ = redis_command_timeout_ms_;
        option.connect_timeout_ms_ = redis_connect_timeout_ms_;
        option.name_ = "game-world-db";
        redis_ = std::make_shared<yuan::redis::RedisClient>(option);
        if (world_ownership_store_ == "redis") {
            world_context_.ownership_store = std::make_shared<RedisWorldOwnershipStore>(redis_);
        } else if (world_ownership_store_ == "memory") {
            world_context_.ownership_store = std::make_shared<InMemoryWorldOwnershipStore>();
        } else {
            LOG_ERROR("world invalid ownership store mode={}", world_ownership_store_);
            return false;
        }
        register_builtin_gm_commands();
        world_context_.before_login_options = [this](PlayerUid player_uid) {
            ensure_player_roles(player_uid);
        };
        world_context_.after_player_zone_set = [this](PlayerId player_id, PackedGameServiceId zone_service_id) {
            mark_role_dirty(player_id, zone_service_id);
        };
        world_context_.gm_forward_handler = [this](SSGmCommandRequest request) {
            return forward_gm(std::move(request));
        };
        if (!register_world_msg(world_rpc_, world_context_)) {
            return false;
        }
        ok_ = rpc_server_.start(rpc_network::RpcNetworkServerConfig{listen_host_, port_, 0}, world_rpc_);
        if (ok_ && http_port_ != 0) {
            yuan::net::http::HttpServerConfig http_config;
            http_config.enable_keep_alive = false;
            http_config.server_name = "GameWorld/1.0";
            http_server_ = std::make_unique<yuan::net::http::HttpServer>(http_config);
            register_http_routes();
            ok_ = http_server_->init(http_port_);
        }
        return ok_;
    }

    void WorldServerService::start()
    {
        draining_.store(false, std::memory_order_relaxed);
        messaging_.start_heartbeat();
        flush_thread_ = std::jthread([this](std::stop_token stop_token) {
            flush_loop(stop_token);
        });
        if (metrics_log_interval_ms_ != 0) {
            metrics_thread_ = std::jthread([this](std::stop_token stop_token) {
                metrics_loop(stop_token);
            });
        }
        register_thread_ = std::jthread([this](std::stop_token stop_token) {
            register_loop(stop_token);
        });
        if (http_server_) {
            http_thread_ = std::jthread([this](std::stop_token) {
                http_server_->serve();
            });
        }
        ok_ = ok_ && rpc_server_.run();
    }

    void WorldServerService::stop()
    {
        draining_.store(true, std::memory_order_relaxed);
        messaging_.stop_heartbeat();
        if (http_server_) {
            http_server_->stop();
        }
        if (http_thread_.joinable()) {
            http_thread_.join();
        }
        flush_thread_.request_stop();
        metrics_thread_.request_stop();
        register_thread_.request_stop();
        if (flush_thread_.joinable()) {
            flush_thread_.join();
        }
        if (register_thread_.joinable()) {
            register_thread_.join();
        }
        if (metrics_thread_.joinable()) {
            metrics_thread_.join();
        }
        flush_dirty_roles();
        const auto metrics = messaging_.metrics();
        LOG_INFO("world messaging metrics tunnel_attempts={} tunnel_retries={} tunnel_recoveries={} tunnel_failures={}",
                 metrics.tunnel_call_attempts,
                 metrics.tunnel_call_retries,
                 metrics.tunnel_call_recoveries,
                 metrics.tunnel_call_failures);
        rpc_server_.stop();
    }

    bool WorldServerService::ok() const
    {
        return ok_;
    }

    bool WorldServerService::register_to_tunnel()
    {
        TunnelRegistration registration;
        registration.service_id = service_id_.pack();
        registration.host = listen_host_;
        registration.port = port_;
        registration.name = "world";
        auto response = messaging_.register_service(std::move(registration));
        return response && response->status == yuan::rpc::RpcStatus::ok;
    }

    void WorldServerService::register_loop(std::stop_token stop_token)
    {
        while (!stop_token.stop_requested()) {
            if (register_to_tunnel()) {
                (void)wait_for_stop(stop_token, std::chrono::seconds{5});
            } else {
                LOG_ERROR("world failed to register to tunnel service_id={}", service_id_.pack());
                (void)wait_for_stop(stop_token, std::chrono::milliseconds{500});
            }
        }
    }

    void WorldServerService::register_http_routes()
    {
        if (!http_server_) {
            return;
        }
        http_server_->use([](yuan::net::http::HttpRequest *, yuan::net::http::HttpResponse *response) {
            response->set_response_code(yuan::net::http::ResponseCode::ok_);
            return yuan::net::http::MiddlewareResult::next;
        }, "game_world_default_ok");
        http_server_->on("/game/login_options", [this](yuan::net::http::HttpRequest *request,
                                                        yuan::net::http::HttpResponse *response) {
            if (!request->is_get()) {
                response->json(encode_http_error_json("method not allowed"), yuan::net::http::ResponseCode::method_not_allowed);
                return;
            }
            const auto player_uid_text = request->get_param("player_uid");
            if (draining_.load(std::memory_order_relaxed)) {
                response->json(encode_http_error_json("world is draining"), yuan::net::http::ResponseCode::service_unavailable);
                return;
            }
            if (player_uid_text.empty()) {
                response->json(encode_http_error_json("missing player_uid"), yuan::net::http::ResponseCode::bad_request);
                return;
            }
            PlayerUid player_uid = 0;
            try {
                player_uid = static_cast<PlayerUid>(std::stoull(player_uid_text));
            } catch (...) {
                response->json(encode_http_error_json("invalid player_uid"), yuan::net::http::ResponseCode::bad_request);
                return;
            }
            ensure_player_roles(player_uid);
            response->json(encode_login_options_response_json(world_login_options(world_context_, player_uid)));
        });
    }

    void WorldServerService::ensure_player_roles(PlayerUid player_uid)
    {
        roles_.ensure_player_roles(player_uid, redis_.get(), service_id_.pack(), [this](PlayerUid uid, const SSPlayerRoleInfo &role) {
            world_add_role(world_context_, uid, role);
        });
    }

    void WorldServerService::mark_role_dirty(PlayerId player_id, PackedGameServiceId zone_service_id)
    {
        roles_.mark_role_zone(player_id, zone_service_id);
        if (!world_db_proxy_routing_.endpoints.empty()) {
            std::scoped_lock lock(pending_role_locations_mutex_);
            pending_role_locations_[player_id] = zone_service_id;
        }
    }

    bool WorldServerService::save_role_location_to_db(PlayerId player_id, PackedGameServiceId zone_service_id) const
    {
        const auto target_proxy = select_db_proxy(player_id, world_db_proxy_routing_);
        if (!target_proxy) {
            return true;
        }
        const auto online_it = world_context_.online_by_role.find(player_id);
        const auto player_uid = online_it != world_context_.online_by_role.end() ? online_it->second.player_uid : player_uid_for_role(player_id);
        const auto gateway_session_id = online_it != world_context_.online_by_role.end() ? online_it->second.gateway_session_id : 0;
        yuan::rpc::Bytes payload;
        SSWorldDbRoleLocationSetRequest request;
        request.player_uid = player_uid;
        request.role_id = player_id;
        request.zone_service_id = zone_service_id;
        request.gateway_session_id = gateway_session_id;
        request.data_version = 0;
        if (!encode_binary(request, payload)) {
            return false;
        }
        auto response = messaging_.send_to_service(service_id_.pack(), *target_proxy, game_route::world_db_role_location_set(), std::move(payload));
        const bool ok = response && response->status == yuan::rpc::RpcStatus::ok;
        if (!ok) {
            LOG_ERROR("world role location save via world_db_proxy failed role_id={} proxy_service={} status={}",
                      player_id,
                      *target_proxy,
                      response ? static_cast<int>(response->status) : -1);
        }
        return ok;
    }

    PlayerUid WorldServerService::player_uid_for_role(PlayerId player_id) const
    {
        for (const auto &[uid, roles] : world_context_.roles_by_player_uid) {
            for (const auto &role : roles) {
                if (role.role_id == player_id) {
                    return uid;
                }
            }
        }
        return 0;
    }

    std::optional<SSGmCommandResponse> WorldServerService::forward_gm(SSGmCommandRequest request) const
    {
        const auto definition = GmCommandRegistry::instance().find(request.command);
        if (!definition) {
            return SSGmCommandResponse{false, "unknown gm command: " + request.command};
        }
        if (request.target_service_id == 0) {
            request.target_service_id = pack_game_service_id(service_id_.region, service_id_.world, definition->executor_type, 1);
        }

        const auto route = gm_execute_route_for(definition->executor_type);
        if (!route) {
            return SSGmCommandResponse{false, "gm executor type is not routable"};
        }

        yuan::rpc::Bytes gm_payload;
        if (!encode_binary(request, gm_payload)) {
            return SSGmCommandResponse{false, "failed to encode gm command"};
        }

        auto response = messaging_.send_to_service(service_id_.pack(),
                                                   request.target_service_id,
                                                   *route,
                                                   std::move(gm_payload));
        if (!response) {
            return std::nullopt;
        }
        const auto result = decode_binary<SSGmCommandResponse>(response->payload);
        if (!result) {
            return SSGmCommandResponse{false, response->error.empty() ? "gm response decode failed" : response->error};
        }
        return result;
    }

    void WorldServerService::flush_dirty_roles()
    {
        roles_.flush_dirty(redis_.get(), service_id_.pack());
        std::unordered_map<PlayerId, PackedGameServiceId> pending_locations;
        {
            std::scoped_lock lock(pending_role_locations_mutex_);
            pending_locations.swap(pending_role_locations_);
        }
        for (const auto &[player_id, zone_service_id] : pending_locations) {
            if (!save_role_location_to_db(player_id, zone_service_id)) {
                std::scoped_lock lock(pending_role_locations_mutex_);
                pending_role_locations_[player_id] = zone_service_id;
            }
        }
    }

    void WorldServerService::flush_loop(std::stop_token stop_token)
    {
        const auto interval = std::chrono::milliseconds(redis_flush_interval_ms_ == 0 ? 5000 : redis_flush_interval_ms_);
        while (!stop_token.stop_requested()) {
            if (wait_for_stop(stop_token, interval)) {
                break;
            }
            flush_dirty_roles();
        }
    }

    void WorldServerService::metrics_loop(std::stop_token stop_token) const
    {
        const auto interval = std::chrono::milliseconds(metrics_log_interval_ms_);
        while (!stop_token.stop_requested()) {
            if (wait_for_stop(stop_token, interval)) {
                break;
            }
            const auto metrics = messaging_.metrics();
            LOG_INFO("world messaging metrics tunnel_attempts={} tunnel_retries={} tunnel_recoveries={} tunnel_failures={}",
                     metrics.tunnel_call_attempts,
                     metrics.tunnel_call_retries,
                     metrics.tunnel_call_recoveries,
                     metrics.tunnel_call_failures);
        }
    }
}
