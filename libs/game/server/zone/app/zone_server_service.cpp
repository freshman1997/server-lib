#include "zone/app/zone_server_service.h"

#include "internal/def.h"
#include "logger.h"
#include "option.h"
#include "redis_client.h"
#include "common/proto/player_db_proto.h"
#include "value/null_value.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <utility>

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
    }

    ZoneServerService::ZoneServerService(GameServiceId service_id,
                                               GameServiceId world_service_id,
                                               GameServiceId player_db_proxy_service_id,
                                               DbProxyRoutingConfig player_db_proxy_routing,
                                               std::string listen_host,
                                              std::vector<rpc_network::RpcEndpoint> tunnel_endpoints,
                                             std::uint16_t listen_port,
                                            std::string redis_host,
                                            std::uint16_t redis_port,
                                            std::uint16_t redis_db,
                                            std::string redis_username,
                                            std::string redis_password,
                                             std::uint16_t redis_connect_timeout_ms,
                                             std::uint16_t redis_command_timeout_ms,
                                             std::uint16_t redis_flush_interval_ms,
                                             std::uint16_t zone_load_sync_interval_ms,
                                              std::uint32_t zone_max_players,
                                              std::uint64_t tunnel_heartbeat_interval_ms,
                                              WorldRoutingConfig world_routing,
                                              std::vector<SSGatewayInfo> gateway_endpoints)
        : listen_host_(std::move(listen_host)),
          listen_port_(listen_port),
          service_id_(service_id),
          world_service_id_(world_service_id),
          player_db_proxy_service_id_(player_db_proxy_service_id),
          player_db_proxy_routing_(std::move(player_db_proxy_routing)),
          messaging_(std::move(tunnel_endpoints)),
          zone_address_(ServiceAddress{service_id, 200, yuan::game_base::ServerRole::scene, service_id.world, "zone"}),
          redis_host_(std::move(redis_host)),
          redis_port_(redis_port),
          redis_db_(redis_db),
          redis_username_(std::move(redis_username)),
          redis_password_(std::move(redis_password)),
          redis_connect_timeout_ms_(redis_connect_timeout_ms),
          redis_command_timeout_ms_(redis_command_timeout_ms),
          redis_flush_interval_ms_(redis_flush_interval_ms),
          zone_load_sync_interval_ms_(zone_load_sync_interval_ms),
          zone_max_players_(zone_max_players),
          world_routing_(std::move(world_routing)),
          gateway_endpoints_(std::move(gateway_endpoints))
    {
        messaging_.set_heartbeat_interval_ms(tunnel_heartbeat_interval_ms);
    }

    void ZoneServerService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool ZoneServerService::init()
    {
        const bool registered = register_zone_msg_echo(zone_rpc_, zone_address_) &&
                                register_zone_msg_player(zone_rpc_, zone_address_, ZoneMsgPlayerHandlers{
                                    [this](SSPlayerZoneUpdate update) { return update_world_player_zone(update); },
                                     [this](SSGatewayLoginRequest request) { return player_enter(request); },
                                     [this](SSGatewayLoginRequest request) { return player_leave(request); },
                                     [this](std::uint64_t gateway_session_id) { return players_.role_for_gateway_session(gateway_session_id); },
                                     [this](RoleId role_id) { return players_.player_uid_for_role(role_id); }}) &&
                                register_zone_msg_gm(zone_rpc_, [this](SSGmCommandRequest request) {
                                    return execute_gm(std::move(request));
                                });
        if (!registered) {
            return false;
        }
        yuan::redis::Option option;
        option.host_ = redis_host_;
        option.port_ = redis_port_;
        option.db_ = redis_db_;
        option.username_ = redis_username_;
        option.password_ = redis_password_;
        option.timeout_ms_ = redis_command_timeout_ms_;
        option.command_timeout_ms_ = redis_command_timeout_ms_;
        option.connect_timeout_ms_ = redis_connect_timeout_ms_;
        option.name_ = "game-zone-player-cache";
        redis_ = std::make_shared<yuan::redis::RedisClient>(option);
        ok_ = !messaging_.empty() && rpc_server_.start(rpc_network::RpcNetworkServerConfig{listen_host_, listen_port_, 0}, zone_rpc_);
        return ok_;
    }

    void ZoneServerService::start()
    {
        messaging_.start_heartbeat();
        flush_thread_ = std::jthread([this](std::stop_token stop_token) {
            flush_loop(stop_token);
        });
        load_sync_thread_ = std::jthread([this](std::stop_token stop_token) {
            load_sync_loop(stop_token);
        });
        register_thread_ = std::jthread([this](std::stop_token stop_token) {
            register_loop(stop_token);
        });
        if (!register_to_world()) {
            ok_ = false;
            return;
        }
        ok_ = rpc_server_.run();
    }

    void ZoneServerService::stop()
    {
        messaging_.stop_heartbeat();
        flush_thread_.request_stop();
        load_sync_thread_.request_stop();
        register_thread_.request_stop();
        if (flush_thread_.joinable()) {
            flush_thread_.join();
        }
        if (load_sync_thread_.joinable()) {
            load_sync_thread_.join();
        }
        if (register_thread_.joinable()) {
            register_thread_.join();
        }
        flush_dirty_players();
        rpc_server_.stop();
    }

    bool ZoneServerService::ok() const
    {
        return ok_;
    }

    bool ZoneServerService::register_to_tunnel()
    {
        TunnelRegistration registration;
        registration.service_id = service_id_.pack();
        registration.host = listen_host_;
        registration.port = listen_port_;
        registration.name = "zone";
        auto response = messaging_.register_service(std::move(registration));
        return response && response->status == yuan::rpc::RpcStatus::ok;
    }

    void ZoneServerService::register_loop(std::stop_token stop_token)
    {
        while (!stop_token.stop_requested()) {
            if (register_to_tunnel()) {
                (void)wait_for_stop(stop_token, std::chrono::seconds{5});
            } else {
                LOG_ERROR("zone failed to register to tunnel service_id={}", service_id_.pack());
                (void)wait_for_stop(stop_token, std::chrono::milliseconds{500});
            }
        }
    }

    bool ZoneServerService::register_to_world()
    {
        if (world_service_id_.instance == 0) {
            return true;
        }
        return report_zone_load();
    }

    bool ZoneServerService::report_zone_load() const
    {
        if (world_service_id_.instance == 0) {
            return true;
        }
        SSZoneInfo zone_info{service_id_.pack(), "zone", static_cast<std::uint32_t>(players_.online_count()), zone_max_players_, true, gateway_endpoints_};
        zone_info.world_routing_strategy = world_routing_.strategy;
        zone_info.world_routing_version = world_routing_.version;
        zone_info.world_count = world_routing_.world_count;
        yuan::rpc::Bytes world_payload;
        if (!encode_binary(zone_info, world_payload)) {
            return false;
        }
        bool ok = true;
        for (std::uint16_t index = 0; index < world_routing_.world_count; ++index) {
            auto target_world = world_service_id_;
            target_world.world = static_cast<std::uint16_t>(1 + index);
            target_world.shard = target_world.world;
            yuan::rpc::Bytes payload = world_payload;
            auto response = messaging_.send_to_service(service_id_.pack(),
                                                       target_world.pack(),
                                                       game_route::world_zone_register(),
                                                       std::move(payload));
            ok = ok && response && response->status == yuan::rpc::RpcStatus::ok;
        }
        return ok;
    }

    bool ZoneServerService::update_world_player_zone(SSPlayerZoneUpdate update) const
    {
        const auto target_world = route_world_service_by_player_uid(update.player_uid,
                                                                    world_routing_,
                                                                    service_id_.region,
                                                                    world_service_id_.instance);
        if (!target_world) {
            return false;
        }
        yuan::rpc::Bytes world_payload;
        if (!encode_binary(update, world_payload)) {
            return false;
        }
        auto response = messaging_.send_to_service(service_id_.pack(),
                                                   target_world->pack(),
                                                   game_route::world_player_zone_set(),
                                                   std::move(world_payload));
        return response && response->status == yuan::rpc::RpcStatus::ok;
    }

    bool ZoneServerService::player_enter(SSGatewayLoginRequest request)
    {
        if (zone_max_players_ != 0 && !players_.role_online(request.role_id) && players_.online_count() >= zone_max_players_) {
            LOG_ERROR("zone rejected player enter over capacity zone_service={} role_id={} online_players={} max_players={}",
                      service_id_.pack(),
                      request.role_id,
                      players_.online_count(),
                      zone_max_players_);
            return false;
        }
        if (!players_.online(request, [this](SSGatewayLoginRequest load_request) { return load_player_from_db(load_request); })) {
            return false;
        }
        return true;
    }

    bool ZoneServerService::player_leave(SSGatewayLoginRequest request)
    {
        if (request.role_id == 0 && request.gateway_session_id != 0) {
            request.role_id = players_.role_for_gateway_session(request.gateway_session_id);
        }
        if (!players_.offline(request)) {
            return false;
        }
        return true;
    }

    SSGmCommandResponse ZoneServerService::execute_gm(SSGmCommandRequest request)
    {
        if (request.command == "set_player_level") {
            if (request.args.size() != 2) {
                return SSGmCommandResponse{false, "usage: set_player_level <role_id> <level>"};
            }
            const auto role_id = static_cast<RoleId>(std::stoull(request.args[0]));
            const auto level = static_cast<std::uint32_t>(std::stoul(request.args[1]));
            if (!players_.set_level(role_id, level)) {
                return SSGmCommandResponse{false, "role is not online or level is invalid"};
            }
            return SSGmCommandResponse{true, "player level set role=" + std::to_string(role_id) + " level=" + std::to_string(level)};
        }
        return SSGmCommandResponse{false, "unknown zone gm command: " + request.command};
    }

    void ZoneServerService::flush_dirty_players()
    {
        players_.flush_dirty([this](const Player &player) { return save_player_to_db(player); }, service_id_.pack());
    }

    std::optional<Player> ZoneServerService::load_player_from_db(SSGatewayLoginRequest request) const
    {
        const auto target_proxy = select_db_proxy(request.role_id, player_db_proxy_routing_).value_or(player_db_proxy_service_id_.pack());
        if (target_proxy != 0) {
            yuan::rpc::Bytes payload;
            if (encode_binary(SSPlayerDbLoadRoleRequest{request.player_uid, request.role_id}, payload)) {
                auto response = messaging_.send_to_service(service_id_.pack(),
                                                           target_proxy,
                                                           game_route::player_db_load_role(),
                                                           std::move(payload));
                if (response && response->status == yuan::rpc::RpcStatus::ok) {
                    const auto decoded = decode_binary<SSPlayerDbRoleResponse>(response->payload);
                    if (decoded && decoded->ok && decoded->has_role) {
                        return Player{decoded->role.player_uid, decoded->role.role_id, request.gateway_session_id, decoded->role.level, decoded->role.exp};
                    }
                    if (decoded && decoded->ok && !decoded->has_role) {
                        return std::nullopt;
                    }
                }
                LOG_ERROR("zone player load via player_db_proxy failed role_id={} proxy_service={} status={}",
                          request.role_id,
                          target_proxy,
                          response ? static_cast<int>(response->status) : -1);
            }
        }
        return load_player_from_redis(request);
    }

    bool ZoneServerService::save_player_to_db(const Player &player) const
    {
        const auto target_proxy = select_db_proxy(player.role_id, player_db_proxy_routing_).value_or(player_db_proxy_service_id_.pack());
        if (target_proxy != 0) {
            yuan::rpc::Bytes payload;
            SSPlayerDbSaveRoleRequest request;
            request.role = SSPlayerRoleData{player.player_uid, player.role_id, player.level, player.exp};
            request.data_version = 0;
            if (encode_binary(request, payload)) {
                auto response = messaging_.send_to_service(service_id_.pack(),
                                                           target_proxy,
                                                           game_route::player_db_save_role(),
                                                           std::move(payload));
                if (response && response->status == yuan::rpc::RpcStatus::ok) {
                    return true;
                }
                LOG_ERROR("zone player save via player_db_proxy failed role_id={} proxy_service={} status={}",
                          player.role_id,
                          target_proxy,
                          response ? static_cast<int>(response->status) : -1);
            }
        }
        return save_player_to_redis(player);
    }

    std::optional<Player> ZoneServerService::load_player_from_redis(SSGatewayLoginRequest request) const
    {
        if (!redis_ || !redis_->ensure_connected()) {
            return std::nullopt;
        }
        const auto value = redis_->get("game:zone:player:" + std::to_string(request.role_id));
        if (value && value->get_type() != yuan::redis::resp_null) {
            if (auto data = Player::from_json(value->to_string())) {
                return *data;
            }
            if (auto data = Player::from_legacy_text(value->to_string())) {
                return *data;
            }
        }
        return std::nullopt;
    }

    bool ZoneServerService::save_player_to_redis(const Player &player) const
    {
        if (!redis_ || !redis_->ensure_connected()) {
            LOG_ERROR("zone player fallback redis save failed: redis unavailable role_id={}", player.role_id);
            return false;
        }
        const auto saved = redis_->set("game:zone:player:" + std::to_string(player.role_id), player.to_json());
        const bool ok = saved && saved->get_type() != yuan::redis::resp_error && saved->to_string() == "OK";
        if (!ok) {
            LOG_ERROR("zone player fallback redis save failed role_id={} result={}", player.role_id, saved ? saved->to_string() : "null");
        }
        return ok;
    }

    void ZoneServerService::flush_loop(std::stop_token stop_token)
    {
        const auto interval = std::chrono::milliseconds(redis_flush_interval_ms_ == 0 ? 5000 : redis_flush_interval_ms_);
        while (!stop_token.stop_requested()) {
            if (wait_for_stop(stop_token, interval)) {
                break;
            }
            flush_dirty_players();
        }
    }

    void ZoneServerService::load_sync_loop(std::stop_token stop_token)
    {
        const auto interval = std::chrono::milliseconds(zone_load_sync_interval_ms_ == 0 ? 1000 : zone_load_sync_interval_ms_);
        while (!stop_token.stop_requested()) {
            if (wait_for_stop(stop_token, interval)) {
                break;
            }
            if (!report_zone_load()) {
                LOG_ERROR("zone failed to report periodic load zone_service={} online_players={}",
                          service_id_.pack(),
                          players_.online_count());
            }
        }
    }
}
