#include "world/world_server_service.h"

#include "common/gm_command_registry.h"
#include "internal/def.h"
#include "option.h"
#include "redis_client.h"

#include <chrono>
#include <random>
#include <sstream>
#include <vector>

namespace yuan::game::server
{
    namespace
    {
        std::vector<std::string> split_lines(const std::string &text)
        {
            std::vector<std::string> result;
            std::stringstream stream(text);
            std::string item;
            while (std::getline(stream, item, '\n')) {
                if (!item.empty()) {
                    result.push_back(std::move(item));
                }
            }
            return result;
        }
    }

    WorldServerService::WorldServerService(GameServiceId service_id,
                                           std::uint16_t port,
                                           std::uint16_t tunnel_port,
                                           std::string redis_host,
                                           std::uint16_t redis_port,
                                           std::uint16_t redis_db,
                                           std::string redis_username,
                                           std::string redis_password,
                                           std::uint16_t redis_connect_timeout_ms,
                                           std::uint16_t redis_command_timeout_ms,
                                           std::uint16_t redis_flush_interval_ms,
                                           std::size_t expected_requests)
        : port_(port),
          tunnel_port_(tunnel_port),
          expected_requests_(expected_requests),
          service_id_(service_id),
          redis_host_(std::move(redis_host)),
          redis_port_(redis_port),
          redis_db_(redis_db),
          redis_username_(std::move(redis_username)),
          redis_password_(std::move(redis_password)),
          redis_connect_timeout_ms_(redis_connect_timeout_ms),
          redis_command_timeout_ms_(redis_command_timeout_ms),
          redis_flush_interval_ms_(redis_flush_interval_ms),
          world_({service_id, 400, yuan::game_base::ServerRole::world, service_id.world, "world"}),
          messaging_({tunnel_port})
    {
        const auto default_zone = pack_game_service_id(service_id.region, service_id.world, GameServiceType::zone, 1);
        world_.register_zone(ZoneInfo{default_zone, "zone-1", 0, 0, true});
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
        register_builtin_gm_commands();
        world_.set_before_login_options([this](PlayerUid player_uid) {
            ensure_player_roles(player_uid);
        });
        world_.set_after_player_zone_set([this](PlayerId player_id, PackedGameServiceId zone_service_id) {
            mark_role_dirty(player_id, zone_service_id);
        });
        world_.set_gm_forward_handler([this](GmCommandRequest request) {
            return forward_gm(std::move(request));
        });
        ok_ = rpc_server_.bind_loopback(port_, world_.rpc_server(), expected_requests_);
        return ok_;
    }

    void WorldServerService::start()
    {
        messaging_.start_heartbeat();
        flush_thread_ = std::jthread([this](std::stop_token stop_token) {
            flush_loop(stop_token);
        });
        ok_ = ok_ && register_to_tunnel() && rpc_server_.run();
    }

    void WorldServerService::stop()
    {
        messaging_.stop_heartbeat();
        flush_thread_.request_stop();
        if (flush_thread_.joinable()) {
            flush_thread_.join();
        }
        flush_dirty_roles();
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
        registration.port = port_;
        registration.name = "world";
        auto response = messaging_.register_service(std::move(registration));
        return response && response->status == yuan::rpc::RpcStatus::ok;
    }

    void WorldServerService::ensure_player_roles(PlayerUid player_uid)
    {
        if (player_uid == 0 || !redis_ || !redis_->ensure_connected()) {
            return;
        }
        {
            std::scoped_lock lock(cache_mutex_);
            if (loaded_players_.contains(player_uid)) {
                return;
            }
        }

        const auto roles_key = "game:player:" + std::to_string(player_uid) + ":roles";
        const auto role_list = redis_->get(roles_key);
        std::vector<PlayerRoleInfo> loaded_roles;
        if (role_list && role_list->get_type() != yuan::redis::resp_null) {
            for (const auto &role_id_text : split_lines(role_list->to_string())) {
                const auto role_id = static_cast<RoleId>(std::stoull(role_id_text));
                const auto role = load_role(role_id);
                if (role) {
                    loaded_roles.push_back(*role);
                }
            }
        }

        if (loaded_roles.empty()) {
            loaded_roles.push_back(create_role(player_uid));
        }

        {
            std::scoped_lock lock(cache_mutex_);
            if (loaded_players_.contains(player_uid)) {
                return;
            }
            auto &role_ids = role_ids_by_player_uid_[player_uid];
            for (const auto &role : loaded_roles) {
                role_ids.push_back(role.role_id);
                roles_by_id_[role.role_id] = role;
                world_.add_role(player_uid, role);
                if (!role_list || role_list->get_type() == yuan::redis::resp_null || role_list->to_string().empty()) {
                    dirty_roles_.insert(role.role_id);
                }
            }
            if (!role_list || role_list->get_type() == yuan::redis::resp_null || role_list->to_string().empty()) {
                dirty_players_.insert(player_uid);
            }
            loaded_players_.insert(player_uid);
        }
    }

    std::optional<PlayerRoleInfo> WorldServerService::load_role(RoleId role_id) const
    {
        if (!redis_) {
            return std::nullopt;
        }
        const auto value = redis_->get("game:role:" + std::to_string(role_id));
        if (!value || value->get_type() == yuan::redis::resp_null) {
            return std::nullopt;
        }
        const auto fields = split_lines(value->to_string());
        if (fields.size() < 5) {
            return std::nullopt;
        }
        return PlayerRoleInfo{static_cast<RoleId>(std::stoull(fields[0])),
                              fields[1],
                              static_cast<std::uint32_t>(std::stoul(fields[2])),
                              static_cast<PackedGameServiceId>(std::stoull(fields[3])),
                              static_cast<PackedGameServiceId>(std::stoull(fields[4]))};
    }

    PlayerRoleInfo WorldServerService::create_role(PlayerUid player_uid) const
    {
        const auto allocated = redis_ ? redis_->incr("game:role:next_id") : nullptr;
        const auto role_id = allocated ? static_cast<RoleId>(std::stoull(allocated->to_string())) : static_cast<RoleId>(player_uid * 100 + 1);
        return PlayerRoleInfo{role_id, random_role_name(), 1, service_id_.pack(), 0};
    }

    std::string WorldServerService::random_role_name() const
    {
        static constexpr const char *prefixes[] = {"Brave", "Silent", "Crimson", "Azure", "Lucky", "Iron"};
        static constexpr const char *suffixes[] = {"Fox", "Wolf", "Star", "Blade", "River", "Falcon"};
        std::random_device device;
        std::mt19937 rng(device());
        std::uniform_int_distribution<std::size_t> prefix_dist(0, std::size(prefixes) - 1);
        std::uniform_int_distribution<std::size_t> suffix_dist(0, std::size(suffixes) - 1);
        std::uniform_int_distribution<int> number_dist(1000, 9999);
        return std::string(prefixes[prefix_dist(rng)]) + suffixes[suffix_dist(rng)] + std::to_string(number_dist(rng));
    }

    void WorldServerService::mark_role_dirty(PlayerId player_id, PackedGameServiceId zone_service_id)
    {
        std::scoped_lock lock(cache_mutex_);
        const auto it = roles_by_id_.find(player_id);
        if (it != roles_by_id_.end()) {
            it->second.zone_service_id = zone_service_id;
        }
        dirty_roles_.insert(player_id);
    }

    std::optional<GmCommandResponse> WorldServerService::forward_gm(GmCommandRequest request) const
    {
        const auto definition = GmCommandRegistry::instance().find(request.command);
        if (!definition) {
            return GmCommandResponse{false, "unknown gm command: " + request.command};
        }
        if (request.target_service_id == 0) {
            request.target_service_id = pack_game_service_id(service_id_.region, service_id_.world, definition->executor_type, 1);
        }

        const auto route = gm_execute_route_for(definition->executor_type);
        if (!route) {
            return GmCommandResponse{false, "gm executor type is not routable"};
        }

        yuan::rpc::Bytes gm_payload;
        if (!encode_gm_command_request(request, gm_payload)) {
            return GmCommandResponse{false, "failed to encode gm command"};
        }

        auto response = messaging_.send_to_service(service_id_.pack(),
                                                   request.target_service_id,
                                                   *route,
                                                   std::move(gm_payload));
        if (!response) {
            return std::nullopt;
        }
        const auto result = decode_gm_command_response(response->payload);
        if (!result) {
            return GmCommandResponse{false, response->error.empty() ? "gm response decode failed" : response->error};
        }
        return result;
    }

    void WorldServerService::flush_dirty_roles()
    {
        if (!redis_ || !redis_->ensure_connected()) {
            return;
        }

        std::unordered_set<PlayerUid> dirty_players;
        std::unordered_set<RoleId> dirty_roles;
        std::unordered_map<PlayerUid, std::vector<RoleId>> role_ids_by_player_uid;
        std::unordered_map<RoleId, PlayerRoleInfo> roles_by_id;
        {
            std::scoped_lock lock(cache_mutex_);
            dirty_players.swap(dirty_players_);
            dirty_roles.swap(dirty_roles_);
            role_ids_by_player_uid = role_ids_by_player_uid_;
            roles_by_id = roles_by_id_;
        }

        for (const auto player_uid : dirty_players) {
            const auto it = role_ids_by_player_uid.find(player_uid);
            if (it == role_ids_by_player_uid.end()) {
                continue;
            }
            std::string value;
            for (const auto role_id : it->second) {
                if (!value.empty()) {
                    value.push_back('\n');
                }
                value += std::to_string(role_id);
            }
            (void)redis_->set("game:player:" + std::to_string(player_uid) + ":roles", value);
        }

        for (const auto role_id : dirty_roles) {
            const auto it = roles_by_id.find(role_id);
            if (it == roles_by_id.end()) {
                continue;
            }
            const auto &role = it->second;
            const auto value = std::to_string(role.role_id) + "\n" + role.name + "\n" + std::to_string(role.level) + "\n" +
                               std::to_string(role.world_service_id) + "\n" + std::to_string(role.zone_service_id);
            (void)redis_->set("game:role:" + std::to_string(role.role_id), value);
        }
    }

    void WorldServerService::flush_loop(std::stop_token stop_token)
    {
        const auto interval = std::chrono::milliseconds(redis_flush_interval_ms_ == 0 ? 5000 : redis_flush_interval_ms_);
        while (!stop_token.stop_requested()) {
            std::this_thread::sleep_for(interval);
            if (stop_token.stop_requested()) {
                break;
            }
            flush_dirty_roles();
        }
    }
}
