#include "zone/app/zone_server_service.h"

#include "internal/def.h"
#include "logger.h"
#include "option.h"
#include "redis_client.h"

#include <chrono>
#include <string>
#include <thread>
#include <utility>

namespace yuan::game::server
{
    ZoneServerService::ZoneServerService(GameServiceId service_id,
                                            GameServiceId global_service_id,
                                              GameServiceId world_service_id,
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
                                             std::vector<GatewayInfo> gateway_endpoints)
        : listen_host_(std::move(listen_host)),
          listen_port_(listen_port),
          service_id_(service_id),
          global_service_id_(global_service_id),
          world_service_id_(world_service_id),
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
                                    [this](PlayerZoneUpdate update) { return update_world_player_zone(update); },
                                    [this](ClientLoginRequest request) { return player_enter(request); },
                                    [this](ClientLoginRequest request) { return player_leave(request); }}) &&
                                register_zone_msg_gm(zone_rpc_, [this](GmCommandRequest request) {
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
        if (global_service_id_.instance == 0) {
            if (!register_to_world()) {
                ok_ = false;
                return;
            }
            ok_ = rpc_server_.run();
            return;
        }

        bool reverse_ok = false;
        std::thread reverse_listener([this, &reverse_ok] {
            reverse_ok = rpc_server_.run();
        });

        yuan::rpc::Metadata metadata;
        metadata["trace_id"] = "server-smoke";
        auto response = messaging_.send_to_service(service_id_.pack(),
                                                   global_service_id_.pack(),
                                                   game_route::global_echo(),
                                                   yuan::rpc::Codec<std::string>::encode("hello-server-global"),
                                                   std::move(metadata));
        if (!(response && response->status == yuan::rpc::RpcStatus::ok &&
              yuan::rpc::Codec<std::string>::decode(response->payload) == "hello-server-global" &&
              response->metadata.find("global.node") != response->metadata.end() &&
              response->metadata.find("tunnel.instance") != response->metadata.end())) {
            if (reverse_listener.joinable()) {
                rpc_server_.stop();
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
                std::this_thread::sleep_for(std::chrono::seconds{5});
            } else {
                LOG_ERROR("zone failed to register to tunnel service_id={}", service_id_.pack());
                std::this_thread::sleep_for(std::chrono::milliseconds{500});
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
        yuan::rpc::Bytes world_payload;
        if (!encode_zone_info(ZoneInfo{service_id_.pack(), "zone", static_cast<std::uint32_t>(players_.online_count()), zone_max_players_, true, gateway_endpoints_}, world_payload)) {
            return false;
        }
        auto response = messaging_.send_to_service(service_id_.pack(),
                                                   world_service_id_.pack(),
                                                   game_route::world_zone_register(),
                                                   std::move(world_payload));
        return response && response->status == yuan::rpc::RpcStatus::ok;
    }

    bool ZoneServerService::update_world_player_zone(PlayerZoneUpdate update) const
    {
        yuan::rpc::Bytes world_payload;
        if (!encode_player_zone_update(update, world_payload)) {
            return false;
        }
        auto response = messaging_.send_to_service(service_id_.pack(),
                                                   world_service_id_.pack(),
                                                   game_route::world_player_zone_set(),
                                                   std::move(world_payload));
        return response && response->status == yuan::rpc::RpcStatus::ok;
    }

    bool ZoneServerService::player_enter(ClientLoginRequest request)
    {
        if (zone_max_players_ != 0 && !players_.role_online(request.role_id) && players_.online_count() >= zone_max_players_) {
            LOG_ERROR("zone rejected player enter over capacity zone_service={} role_id={} online_players={} max_players={}",
                      service_id_.pack(),
                      request.role_id,
                      players_.online_count(),
                      zone_max_players_);
            return false;
        }
        if (!players_.online(request, redis_.get())) {
            return false;
        }
        return true;
    }

    bool ZoneServerService::player_leave(ClientLoginRequest request)
    {
        if (!players_.offline(request)) {
            return false;
        }
        return true;
    }

    GmCommandResponse ZoneServerService::execute_gm(GmCommandRequest request)
    {
        if (request.command == "set_player_level") {
            if (request.args.size() != 2) {
                return GmCommandResponse{false, "usage: set_player_level <role_id> <level>"};
            }
            const auto role_id = static_cast<RoleId>(std::stoull(request.args[0]));
            const auto level = static_cast<std::uint32_t>(std::stoul(request.args[1]));
            if (!players_.set_level(role_id, level)) {
                return GmCommandResponse{false, "role is not online or level is invalid"};
            }
            return GmCommandResponse{true, "player level set role=" + std::to_string(role_id) + " level=" + std::to_string(level)};
        }
        return GmCommandResponse{false, "unknown zone gm command: " + request.command};
    }

    void ZoneServerService::flush_dirty_players()
    {
        players_.flush_dirty(redis_.get(), service_id_.pack());
    }

    void ZoneServerService::flush_loop(std::stop_token stop_token)
    {
        const auto interval = std::chrono::milliseconds(redis_flush_interval_ms_ == 0 ? 5000 : redis_flush_interval_ms_);
        while (!stop_token.stop_requested()) {
            std::this_thread::sleep_for(interval);
            if (stop_token.stop_requested()) {
                break;
            }
            flush_dirty_players();
        }
    }

    void ZoneServerService::load_sync_loop(std::stop_token stop_token)
    {
        const auto interval = std::chrono::milliseconds(zone_load_sync_interval_ms_ == 0 ? 1000 : zone_load_sync_interval_ms_);
        while (!stop_token.stop_requested()) {
            std::this_thread::sleep_for(interval);
            if (stop_token.stop_requested()) {
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
