#include "zone/app/zone_server_service.h"

#include "internal/def.h"
#include "logger.h"
#include "common/proto/player_db_proto.h"

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

    ZoneServerService::ZoneServerService(ServiceServerConfig config)
        : listen_host_(std::move(config.listen_host)),
          listen_port_(config.listen_port),
          service_id_(config.service_id),
          world_service_id_(config.target_world_id),
          player_db_proxy_service_id_(config.target_player_db_proxy_id),
          player_db_proxy_routing_(std::move(config.player_db_proxy_routing)),
          zone_address_(ServiceAddress{service_id_, 200, yuan::game_base::ServerRole::scene, service_id_.world, "zone"}),
          redis_flush_interval_ms_(config.redis_flush_interval_ms),
          zone_load_sync_interval_ms_(config.zone_load_sync_interval_ms),
          zone_max_players_(config.zone_max_players),
          world_routing_(std::move(config.world_routing)),
          gateway_endpoints_(std::move(config.gateway_endpoints))
    {
        tunnel_client_manager_.set_tunnel_endpoints(config.tunnel_endpoints);
        tunnel_client_manager_.set_heartbeat_interval_ms(config.tunnel_heartbeat_interval_ms);
        tunnel_client_manager_.configure_registered_service(TunnelRegistration{service_id_.pack(), listen_host_, listen_port_, "zone"}, "zone");
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
        ok_ = !tunnel_client_manager_.empty() && rpc_server_.start(rpc_network::RpcNetworkServerConfig{listen_host_, listen_port_, 0}, zone_rpc_);
        return ok_;
    }

    void ZoneServerService::start()
    {
        tunnel_client_manager_.start_registered_service(rpc_server_);
        flush_thread_ = std::jthread([this](std::stop_token stop_token) {
            flush_loop(stop_token);
        });
        const auto load_sync_interval = zone_load_sync_interval_ms_ == 0 ? 1000 : zone_load_sync_interval_ms_;
        load_sync_timer_ = rpc_server_.schedule_periodic(0, load_sync_interval, [this] {
            if (!report_zone_load()) {
                LOG_ERROR("zone failed to report periodic load zone_service={} online_players={}",
                          service_id_.pack(),
                          players_.online_count());
            }
        });
        ok_ = rpc_server_.run();
    }

    void ZoneServerService::stop()
    {
        rpc_server_.stop();
        rpc_server_.cancel_timer(load_sync_timer_);
        flush_thread_.request_stop();
        if (flush_thread_.joinable()) {
            flush_thread_.join();
        }
        for (;;) {
            const auto before = players_.dirty_count();
            const auto remaining = flush_dirty_players();
            if (remaining == 0) {
                break;
            }
            if (remaining >= before) {
                LOG_ERROR("zone stop could not drain dirty players zone_service={} dirty_roles={}", service_id_.pack(), remaining);
                break;
            }
        }
        tunnel_client_manager_.stop_registered_service();
    }

    bool ZoneServerService::ok() const
    {
        return ok_;
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
            auto response = tunnel_client_manager_.send_to_service(service_id_.pack(),
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
        auto response = tunnel_client_manager_.send_to_service(service_id_.pack(),
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

    std::size_t ZoneServerService::flush_dirty_players()
    {
        return players_.flush_dirty([this](const Player &player) { return save_player_to_db(player); }, service_id_.pack());
    }

    std::optional<Player> ZoneServerService::load_player_from_db(SSGatewayLoginRequest request) const
    {
        const auto target_proxy = select_db_proxy(request.role_id, player_db_proxy_routing_).value_or(player_db_proxy_service_id_.pack());
        if (target_proxy == 0) {
            LOG_ERROR("zone player load failed: player_db_proxy unavailable role_id={}", request.role_id);
            return std::nullopt;
        }
        yuan::rpc::Bytes payload;
        if (!encode_binary(SSPlayerDbLoadRoleRequest{request.player_uid, request.role_id}, payload)) {
            return std::nullopt;
        }
        auto response = tunnel_client_manager_.send_to_service(service_id_.pack(),
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
        return std::nullopt;
    }

    bool ZoneServerService::save_player_to_db(const Player &player) const
    {
        const auto target_proxy = select_db_proxy(player.role_id, player_db_proxy_routing_).value_or(player_db_proxy_service_id_.pack());
        if (target_proxy == 0) {
            LOG_ERROR("zone player save failed: player_db_proxy unavailable role_id={}", player.role_id);
            return false;
        }
        yuan::rpc::Bytes payload;
        SSPlayerDbSaveRoleRequest request;
        request.role = SSPlayerRoleData{player.player_uid, player.role_id, player.level, player.exp};
        request.data_version = 0;
        if (!encode_binary(request, payload)) {
            return false;
        }
        auto response = tunnel_client_manager_.send_to_service(service_id_.pack(),
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
        return false;
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

}
