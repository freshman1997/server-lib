#include "world/app/world_server_service.h"

#include "common/gm_command_registry.h"
#include "common/proto/player_db_proto.h"
#include "common/proto/world_db_proto.h"
#include "content_type.h"
#include "header_key.h"
#include "http_headers.h"
#include "middleware.h"
#include "internal/def.h"
#include "logger.h"
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

    WorldServerService::WorldServerService(ServiceServerConfig config)
        : listen_host_(std::move(config.listen_host)),
          port_(config.listen_port),
          http_port_(config.http_port),
          service_id_(config.service_id),
          redis_flush_interval_ms_(config.redis_flush_interval_ms),
          world_ownership_store_(std::move(config.world_ownership_store)),
          metrics_log_interval_ms_(config.metrics_log_interval_ms),
          player_db_proxy_service_id_(config.target_player_db_proxy_id),
          player_db_proxy_routing_(std::move(config.player_db_proxy_routing)),
          world_db_proxy_routing_(std::move(config.world_db_proxy_routing)),
          world_context_({ServiceAddress{service_id_, 400, yuan::game_base::ServerRole::world, service_id_.world, "world"}})
    {
        world_context_.login_reservation_ttl_ms = config.login_reservation_ttl_ms == 0 ? 3000 : config.login_reservation_ttl_ms;
        world_context_.zone_report_ttl_ms = config.zone_report_ttl_ms == 0 ? 3000 : config.zone_report_ttl_ms;
        world_context_.login_token_secret = config.login_token_secret == 0 ? kDefaultLoginTokenSecret : config.login_token_secret;
        world_context_.world_routing = std::move(config.world_routing);
        tunnel_client_manager_.set_tunnel_endpoints(config.tunnel_endpoints);
        tunnel_client_manager_.set_heartbeat_interval_ms(config.tunnel_heartbeat_interval_ms);
        tunnel_client_manager_.configure_registered_service(TunnelRegistration{service_id_.pack(), listen_host_, port_, "world"}, "world");
    }

    void WorldServerService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool WorldServerService::init()
    {
        if (world_ownership_store_ == "proxy" || world_ownership_store_ == "world_db_proxy") {
            world_context_.ownership_store = std::make_shared<WorldDbProxyOwnershipStore>(service_id_.pack(), world_db_proxy_routing_, tunnel_client_manager_);
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
        world_context_.before_login_options_async = [this](PlayerUid player_uid, std::function<void()> done) {
            ensure_player_roles_async_callback(player_uid, std::move(done));
        };
        world_context_.write_deferred_response = [this](std::uint64_t connection_id, yuan::rpc::Response response) {
            (void)rpc_server_.write_response_to_connection(connection_id, std::move(response));
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
            http_config.thread_pool_size = 0;
            http_config.enable_keep_alive = false;
            http_config.server_name = "GameWorld/1.0";
            http_server_ = std::make_unique<yuan::net::http::HttpServer>(http_config);
            register_http_routes();
            ok_ = http_server_->init(http_port_, rpc_server_.runtime());
        }
        return ok_;
    }

    void WorldServerService::start()
    {
        draining_.store(false, std::memory_order_relaxed);
        tunnel_client_manager_.start_registered_service(rpc_server_);
        flush_thread_ = std::jthread([this](std::stop_token stop_token) {
            flush_loop(stop_token);
        });
        if (metrics_log_interval_ms_ != 0) {
            metrics_timer_ = rpc_server_.schedule_periodic(metrics_log_interval_ms_, metrics_log_interval_ms_, [this] {
                log_metrics();
            });
        }
        if (http_server_) {
            http_server_->serve();
        }
        ok_ = ok_ && rpc_server_.run();
    }

    void WorldServerService::stop()
    {
        draining_.store(true, std::memory_order_relaxed);
        tunnel_client_manager_.stop_registered_service();
        if (http_server_) {
            http_server_->stop();
        }
        rpc_server_.cancel_timer(metrics_timer_);
        flush_thread_.request_stop();
        if (flush_thread_.joinable()) {
            flush_thread_.join();
        }
        flush_dirty_roles();
        const auto metrics = tunnel_client_manager_.metrics();
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

    void WorldServerService::register_http_routes()
    {
        if (!http_server_) {
            return;
        }
        http_server_->use([](yuan::net::http::HttpRequest *, yuan::net::http::HttpResponse *response) {
            response->set_response_code(yuan::net::http::ResponseCode::ok_);
            return yuan::net::http::MiddlewareResult::next;
        }, "game_world_default_ok");
        http_server_->on_async("/game/login_options", [this](yuan::net::http::HttpRequest *request,
                                                              yuan::net::http::HttpResponse *response) -> yuan::coroutine::Task<void> {
            if (!request->is_get()) {
                response->json(encode_http_error_json("method not allowed"), yuan::net::http::ResponseCode::method_not_allowed);
                co_return;
            }
            const auto player_uid_text = request->get_param("player_uid");
            if (draining_.load(std::memory_order_relaxed)) {
                response->json(encode_http_error_json("world is draining"), yuan::net::http::ResponseCode::service_unavailable);
                co_return;
            }
            if (player_uid_text.empty()) {
                response->json(encode_http_error_json("missing player_uid"), yuan::net::http::ResponseCode::bad_request);
                co_return;
            }
            PlayerUid player_uid = 0;
            try {
                player_uid = static_cast<PlayerUid>(std::stoull(player_uid_text));
            } catch (...) {
                response->json(encode_http_error_json("invalid player_uid"), yuan::net::http::ResponseCode::bad_request);
                co_return;
            }
            co_await ensure_player_roles_async(player_uid);
            response->json(encode_login_options_response_json(world_login_options(world_context_, player_uid)));
        });
        http_server_->on("/game/create_role", [this](yuan::net::http::HttpRequest *request,
                                                     yuan::net::http::HttpResponse *response) {
            if (!request->is_get()) {
                response->json(encode_http_error_json("method not allowed"), yuan::net::http::ResponseCode::method_not_allowed);
                return;
            }
            PlayerUid player_uid = 0;
            try {
                player_uid = static_cast<PlayerUid>(std::stoull(request->get_param("player_uid")));
            } catch (...) {
                response->json(encode_http_error_json("invalid player_uid"), yuan::net::http::ResponseCode::bad_request);
                return;
            }
            const auto name = request->get_param("name");
            auto role = create_role(player_uid, name);
            if (!role) {
                response->json(encode_http_error_json("create role failed"), yuan::net::http::ResponseCode::bad_request);
                return;
            }
            nlohmann::json root;
            root["ok"] = true;
            root["player_uid"] = player_uid;
            root["role_id"] = role->role_id;
            root["message"] = "created";
            response->json(root.dump());
        });
    }

    void WorldServerService::ensure_player_roles(PlayerUid player_uid)
    {
        if (roles_.is_player_loaded(player_uid)) {
            return;
        }
        const auto loaded = load_player_roles_from_db(player_uid);
        roles_.apply_player_roles(loaded, [this](PlayerUid uid, const SSPlayerRoleInfo &role) {
            world_add_role(world_context_, uid, role);
        });
    }

    yuan::coroutine::Task<void> WorldServerService::ensure_player_roles_async(PlayerUid player_uid)
    {
        ensure_player_roles(player_uid);
        co_return;
    }

    void WorldServerService::ensure_player_roles_async_callback(PlayerUid player_uid, std::function<void()> done)
    {
        ensure_player_roles(player_uid);
        done();
    }

    RoleCache::LoadedPlayerRoles WorldServerService::load_player_roles_from_db(PlayerUid player_uid) const
    {
        RoleCache::LoadedPlayerRoles loaded;
        loaded.player_uid = player_uid;
        const auto target_proxy = select_db_proxy(player_uid, world_db_proxy_routing_);
        if (!target_proxy) {
            LOG_ERROR("world player roles load failed: world_db_proxy unavailable player_uid={}", player_uid);
            return loaded;
        }
        yuan::rpc::Bytes payload;
        SSWorldDbPlayerRolesGetRequest request;
        request.player_uid = player_uid;
        request.world_service_id = service_id_.pack();
        if (!encode_binary(request, payload)) {
            return loaded;
        }
        auto response = tunnel_client_manager_.send_to_service(service_id_.pack(), *target_proxy, game_route::world_db_player_roles_get(), std::move(payload));
        if (!response || response->status != yuan::rpc::RpcStatus::ok) {
            LOG_ERROR("world player roles load via world_db_proxy failed player_uid={} proxy_service={} status={}",
                      player_uid,
                      *target_proxy,
                      response ? static_cast<int>(response->status) : -1);
            return loaded;
        }
        const auto body = decode_binary<SSWorldDbPlayerRolesResponse>(response->payload);
        if (!body || !body->ok) {
            LOG_ERROR("world player roles load decode failed player_uid={} proxy_service={}", player_uid, *target_proxy);
            return loaded;
        }
        loaded.player_uid = body->player_uid;
        loaded.roles = body->roles;
        loaded.created_default_role = false;
        loaded.missing_role_list = body->missing_role_list;
        return loaded;
    }

    std::optional<SSPlayerRoleInfo> WorldServerService::create_role(PlayerUid player_uid, std::string name)
    {
        if (player_uid == 0 || name.empty()) {
            return std::nullopt;
        }
        ensure_player_roles(player_uid);
        if (const auto existing = world_context_.roles_by_player_uid.find(player_uid); existing != world_context_.roles_by_player_uid.end() && !existing->second.empty()) {
            LOG_ERROR("world create role rejected: player already has role player_uid={}", player_uid);
            return std::nullopt;
        }

        const auto role_id = static_cast<RoleId>(player_uid * 100 + 1);
        SSPlayerRoleInfo role{role_id, std::move(name), 1, service_id_.pack(), 0};
        const auto target_player_proxy = select_db_proxy(role_id, player_db_proxy_routing_).value_or(player_db_proxy_service_id_.pack());
        if (target_player_proxy == 0) {
            LOG_ERROR("world create role failed: player_db_proxy unavailable player_uid={}", player_uid);
            return std::nullopt;
        }
        yuan::rpc::Bytes player_payload;
        SSPlayerDbCreateRoleRequest player_request;
        player_request.player_uid = player_uid;
        player_request.role_id = role_id;
        player_request.name = role.name;
        if (!encode_binary(player_request, player_payload)) {
            return std::nullopt;
        }
        auto player_response = tunnel_client_manager_.send_to_service(service_id_.pack(), target_player_proxy, game_route::player_db_create_role(), std::move(player_payload));
        if (!player_response || player_response->status != yuan::rpc::RpcStatus::ok) {
            LOG_ERROR("world create role failed via player_db_proxy player_uid={} role_id={} proxy_service={} status={}",
                      player_uid,
                      role_id,
                      target_player_proxy,
                      player_response ? static_cast<int>(player_response->status) : -1);
            return std::nullopt;
        }
        if (!save_player_roles_to_db(player_uid, {role})) {
            LOG_ERROR("world create role failed: world_db save failed player_uid={} role_id={}", player_uid, role_id);
            return std::nullopt;
        }
        RoleCache::LoadedPlayerRoles loaded;
        loaded.player_uid = player_uid;
        loaded.roles = {role};
        roles_.apply_player_roles(loaded, [this](PlayerUid uid, const SSPlayerRoleInfo &loaded_role) {
            world_add_role(world_context_, uid, loaded_role);
        });
        return role;
    }

    void WorldServerService::mark_role_dirty(PlayerId player_id, PackedGameServiceId zone_service_id)
    {
        roles_.mark_role_zone(player_id, zone_service_id);
        if (!world_db_proxy_routing_.endpoints.empty()) {
            std::scoped_lock lock(pending_role_locations_mutex_);
            pending_role_locations_[player_id] = zone_service_id;
        }
    }

    bool WorldServerService::save_player_roles_to_db(PlayerUid player_uid, const std::vector<SSPlayerRoleInfo> &roles) const
    {
        const auto target_proxy = select_db_proxy(player_uid, world_db_proxy_routing_);
        if (!target_proxy) {
            LOG_ERROR("world player roles save failed: world_db_proxy unavailable player_uid={}", player_uid);
            return false;
        }
        yuan::rpc::Bytes payload;
        SSWorldDbPlayerRolesSaveRequest request;
        request.player_uid = player_uid;
        request.roles = roles;
        request.data_version = 0;
        if (!encode_binary(request, payload)) {
            return false;
        }
        auto response = tunnel_client_manager_.send_to_service(service_id_.pack(), *target_proxy, game_route::world_db_player_roles_save(), std::move(payload));
        const bool ok = response && response->status == yuan::rpc::RpcStatus::ok;
        if (!ok) {
            LOG_ERROR("world player roles save via world_db_proxy failed player_uid={} proxy_service={} status={}",
                      player_uid,
                      *target_proxy,
                      response ? static_cast<int>(response->status) : -1);
        }
        return ok;
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
        auto response = tunnel_client_manager_.send_to_service(service_id_.pack(), *target_proxy, game_route::world_db_role_location_set(), std::move(payload));
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

        auto response = tunnel_client_manager_.send_to_service(service_id_.pack(),
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
        (void)roles_.flush_dirty([this](PlayerUid player_uid, const std::vector<SSPlayerRoleInfo> &roles) {
            return save_player_roles_to_db(player_uid, roles);
        }, service_id_.pack());
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

    void WorldServerService::log_metrics() const
    {
        const auto metrics = tunnel_client_manager_.metrics();
        LOG_INFO("world messaging metrics tunnel_attempts={} tunnel_retries={} tunnel_recoveries={} tunnel_failures={}",
                 metrics.tunnel_call_attempts,
                 metrics.tunnel_call_retries,
                 metrics.tunnel_call_recoveries,
                 metrics.tunnel_call_failures);
    }
}
