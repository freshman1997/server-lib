#include "zone/zone_server_service.h"

#include "internal/def.h"
#include "option.h"
#include "redis_client.h"

#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace yuan::game::server
{
    ZoneServerService::ZoneServerService(GameServiceId service_id,
                                           GameServiceId global_service_id,
                                            GameServiceId world_service_id,
                                            std::uint16_t tunnel_port,
                                            std::uint16_t listen_port,
                                            std::string redis_host,
                                            std::uint16_t redis_port,
                                            std::uint16_t redis_db,
                                            std::string redis_username,
                                            std::string redis_password,
                                            std::uint16_t redis_connect_timeout_ms,
                                            std::uint16_t redis_command_timeout_ms,
                                            std::uint16_t redis_flush_interval_ms,
                                            std::size_t expected_requests)
        : tunnel_port_(tunnel_port),
          listen_port_(listen_port),
          expected_requests_(expected_requests),
          service_id_(service_id),
          global_service_id_(global_service_id),
          world_service_id_(world_service_id),
          messaging_({tunnel_port}),
          zone_(ServiceAddress{service_id, 200, yuan::game_base::ServerRole::scene, service_id.world, "zone"},
                [this](TunnelEnvelope envelope) {
                    auto response = messaging_.forward(std::move(envelope));
                    if (response) {
                        return *response;
                    }
                    yuan::rpc::Response unavailable;
                    unavailable.status = yuan::rpc::RpcStatus::unavailable;
                    unavailable.error = "tunnel unavailable";
                    return unavailable;
                }),
          redis_host_(std::move(redis_host)),
          redis_port_(redis_port),
          redis_db_(redis_db),
          redis_username_(std::move(redis_username)),
          redis_password_(std::move(redis_password)),
          redis_connect_timeout_ms_(redis_connect_timeout_ms),
          redis_command_timeout_ms_(redis_command_timeout_ms),
          redis_flush_interval_ms_(redis_flush_interval_ms)
    {
    }

    void ZoneServerService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool ZoneServerService::init()
    {
        zone_.set_world_zone_update_handler([this](PlayerZoneUpdate update) {
            return update_world_player_zone(update);
        });
        zone_.set_player_enter_handler([this](ClientLoginRequest request) {
            return player_enter(request);
        });
        zone_.set_player_leave_handler([this](ClientLoginRequest request) {
            return player_leave(request);
        });
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
        ok_ = !messaging_.empty() && rpc_server_.bind_loopback(listen_port_, zone_.rpc_server(), expected_requests_);
        return ok_;
    }

    void ZoneServerService::start()
    {
        messaging_.start_heartbeat();
        flush_thread_ = std::jthread([this](std::stop_token stop_token) {
            flush_loop(stop_token);
        });
        if (!register_to_tunnel()) {
            ok_ = false;
            return;
        }
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
        if (flush_thread_.joinable()) {
            flush_thread_.join();
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
        registration.port = listen_port_;
        registration.name = "zone";
        auto response = messaging_.register_service(std::move(registration));
        return response && response->status == yuan::rpc::RpcStatus::ok;
    }

    bool ZoneServerService::register_to_world()
    {
        if (world_service_id_.instance == 0) {
            return true;
        }
        yuan::rpc::Bytes world_payload;
        if (!encode_zone_info(ZoneInfo{service_id_.pack(), "zone", 0, 0, true}, world_payload)) {
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
        if (request.player_uid == 0 || request.role_id == 0) {
            return false;
        }
        auto data = load_or_create_player(request);
        std::scoped_lock lock(player_cache_mutex_);
        players_by_role_[request.role_id] = data;
        return true;
    }

    bool ZoneServerService::player_leave(ClientLoginRequest request)
    {
        if (request.role_id == 0) {
            return false;
        }
        mark_player_dirty(request.role_id);
        return true;
    }

    ZoneServerService::PlayerRuntimeData ZoneServerService::load_or_create_player(ClientLoginRequest request) const
    {
        if (redis_ && redis_->ensure_connected()) {
            const auto value = redis_->get("game:zone:player:" + std::to_string(request.role_id));
            if (value && value->get_type() != yuan::redis::resp_null) {
                std::stringstream stream(value->to_string());
                std::string field;
                PlayerRuntimeData data;
                if (std::getline(stream, field, '\n')) {
                    data.player_uid = static_cast<PlayerUid>(std::stoull(field));
                }
                if (std::getline(stream, field, '\n')) {
                    data.role_id = static_cast<RoleId>(std::stoull(field));
                }
                if (std::getline(stream, field, '\n')) {
                    data.level = static_cast<std::uint32_t>(std::stoul(field));
                }
                if (std::getline(stream, field, '\n')) {
                    data.exp = static_cast<std::uint64_t>(std::stoull(field));
                }
                if (data.player_uid != 0 && data.role_id != 0) {
                    return data;
                }
            }
        }
        return PlayerRuntimeData{request.player_uid, request.role_id, 1, 0};
    }

    void ZoneServerService::mark_player_dirty(RoleId role_id)
    {
        std::scoped_lock lock(player_cache_mutex_);
        if (players_by_role_.contains(role_id)) {
            dirty_roles_.insert(role_id);
        }
    }

    void ZoneServerService::flush_dirty_players()
    {
        if (!redis_ || !redis_->ensure_connected()) {
            return;
        }

        std::unordered_map<RoleId, PlayerRuntimeData> dirty_players;
        {
            std::scoped_lock lock(player_cache_mutex_);
            for (const auto role_id : dirty_roles_) {
                const auto it = players_by_role_.find(role_id);
                if (it != players_by_role_.end()) {
                    dirty_players.emplace(role_id, it->second);
                }
            }
            dirty_roles_.clear();
        }

        for (const auto &[role_id, data] : dirty_players) {
            const auto value = std::to_string(data.player_uid) + "\n" + std::to_string(data.role_id) + "\n" +
                               std::to_string(data.level) + "\n" + std::to_string(data.exp);
            (void)redis_->set("game:zone:player:" + std::to_string(role_id), value);
        }
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
}
