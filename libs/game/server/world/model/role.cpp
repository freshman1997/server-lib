#include "world/model/role.h"

#include "internal/def.h"
#include "logger.h"
#include "redis_client.h"

#include <nlohmann/json.hpp>

#include <random>
#include <sstream>

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

        std::string encode_role_ids_json(const std::vector<RoleId> &role_ids)
        {
            nlohmann::json root;
            root["roles"] = role_ids;
            return root.dump();
        }

        std::optional<std::vector<RoleId>> decode_role_ids_json(const std::string &text)
        {
            try {
                const auto root = nlohmann::json::parse(text);
                std::vector<RoleId> role_ids;
                if (root.is_array()) {
                    role_ids = root.get<std::vector<RoleId>>();
                } else if (root.contains("roles")) {
                    role_ids = root.at("roles").get<std::vector<RoleId>>();
                }
                return role_ids;
            } catch (...) {
            }
            return std::nullopt;
        }

        std::string encode_role_json(const SSPlayerRoleInfo &role)
        {
            nlohmann::json root;
            root["role_id"] = role.role_id;
            root["name"] = role.name;
            root["level"] = role.level;
            root["world_service_id"] = role.world_service_id;
            root["zone_service_id"] = role.zone_service_id;
            return root.dump();
        }

        std::optional<SSPlayerRoleInfo> decode_role_json(const std::string &text)
        {
            try {
                const auto root = nlohmann::json::parse(text);
                SSPlayerRoleInfo role;
                role.role_id = root.value("role_id", static_cast<RoleId>(0));
                role.name = root.value("name", std::string{});
                role.level = root.value("level", static_cast<std::uint32_t>(1));
                role.world_service_id = root.value("world_service_id", static_cast<PackedGameServiceId>(0));
                role.zone_service_id = root.value("zone_service_id", static_cast<PackedGameServiceId>(0));
                if (role.role_id != 0) {
                    return role;
                }
            } catch (...) {
            }
            return std::nullopt;
        }

        bool redis_set_ok(const std::shared_ptr<yuan::redis::RedisValue> &value)
        {
            return value && value->get_type() != yuan::redis::resp_error && value->to_string() == "OK";
        }
    }

    void RoleCache::ensure_player_roles(PlayerUid player_uid,
                                             yuan::redis::RedisClient *redis,
                                             PackedGameServiceId world_service_id,
                                             std::function<void(PlayerUid, const SSPlayerRoleInfo &)> add_role)
    {
        if (player_uid == 0 || !redis || !redis->ensure_connected()) {
            return;
        }
        {
            std::scoped_lock lock(mutex_);
            if (loaded_players_.contains(player_uid)) {
                return;
            }
        }

        const auto roles_key = "game:player:" + std::to_string(player_uid) + ":roles";
        const auto role_list = redis->get(roles_key);
        std::vector<SSPlayerRoleInfo> loaded_roles;
        if (role_list && role_list->get_type() != yuan::redis::resp_null) {
            auto role_ids = decode_role_ids_json(role_list->to_string());
            const auto legacy_role_ids = role_ids ? std::vector<std::string>{} : split_lines(role_list->to_string());
            if (!role_ids) {
                role_ids.emplace();
                for (const auto &role_id_text : legacy_role_ids) {
                    role_ids->push_back(static_cast<RoleId>(std::stoull(role_id_text)));
                }
            }
            for (const auto role_id : *role_ids) {
                const auto role = load_role(role_id, redis);
                if (role) {
                    loaded_roles.push_back(*role);
                }
            }
        }

        if (loaded_roles.empty()) {
            loaded_roles.push_back(create_role(player_uid, redis, world_service_id));
        }

        {
            std::scoped_lock lock(mutex_);
            if (loaded_players_.contains(player_uid)) {
                return;
            }
            auto &role_ids = role_ids_by_player_uid_[player_uid];
            for (const auto &role : loaded_roles) {
                role_ids.push_back(role.role_id);
                roles_by_id_[role.role_id] = role;
                add_role(player_uid, role);
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

    void RoleCache::mark_role_zone(PlayerId player_id, PackedGameServiceId zone_service_id)
    {
        std::scoped_lock lock(mutex_);
        const auto it = roles_by_id_.find(player_id);
        if (it != roles_by_id_.end()) {
            it->second.zone_service_id = zone_service_id;
        }
        dirty_roles_.insert(player_id);
    }

    void RoleCache::flush_dirty(yuan::redis::RedisClient *redis, PackedGameServiceId world_service_id)
    {
        if (!redis || !redis->ensure_connected()) {
            LOG_ERROR("world role flush failed: redis unavailable world_service={} dirty_players={} dirty_roles={}",
                      world_service_id, dirty_player_count(), dirty_role_count());
            return;
        }

        std::unordered_set<PlayerUid> dirty_players;
        std::unordered_set<RoleId> dirty_roles;
        std::unordered_map<PlayerUid, std::vector<RoleId>> role_ids_by_player_uid;
        std::unordered_map<RoleId, SSPlayerRoleInfo> roles_by_id;
        {
            std::scoped_lock lock(mutex_);
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
            const auto key = "game:player:" + std::to_string(player_uid) + ":roles";
            const auto value = encode_role_ids_json(it->second);
            const auto saved = redis->set(key, value);
            if (!redis_set_ok(saved)) {
                LOG_ERROR("world role-list flush failed: key={} player_uid={} redis_result={}",
                          key,
                          player_uid,
                          saved ? saved->to_string() : "null");
            }
        }

        for (const auto role_id : dirty_roles) {
            const auto it = roles_by_id.find(role_id);
            if (it == roles_by_id.end()) {
                continue;
            }
            const auto &role = it->second;
            const auto key = "game:role:" + std::to_string(role.role_id);
            const auto value = encode_role_json(role);
            const auto saved = redis->set(key, value);
            if (!redis_set_ok(saved)) {
                LOG_ERROR("world role flush failed: key={} role_id={} player_world={} redis_result={}",
                          key,
                          role.role_id,
                          role.world_service_id,
                          saved ? saved->to_string() : "null");
            }
        }
    }

    std::size_t RoleCache::dirty_player_count() const
    {
        std::scoped_lock lock(mutex_);
        return dirty_players_.size();
    }

    std::size_t RoleCache::dirty_role_count() const
    {
        std::scoped_lock lock(mutex_);
        return dirty_roles_.size();
    }

    std::optional<SSPlayerRoleInfo> RoleCache::load_role(RoleId role_id, yuan::redis::RedisClient *redis) const
    {
        if (!redis) {
            return std::nullopt;
        }
        const auto value = redis->get("game:role:" + std::to_string(role_id));
        if (!value || value->get_type() == yuan::redis::resp_null) {
            return std::nullopt;
        }
        if (const auto role = decode_role_json(value->to_string())) {
            return role;
        }
        const auto fields = split_lines(value->to_string());
        if (fields.size() < 5) {
            return std::nullopt;
        }
        return SSPlayerRoleInfo{static_cast<RoleId>(std::stoull(fields[0])),
                              fields[1],
                              static_cast<std::uint32_t>(std::stoul(fields[2])),
                              static_cast<PackedGameServiceId>(std::stoull(fields[3])),
                              static_cast<PackedGameServiceId>(std::stoull(fields[4]))};
    }

    SSPlayerRoleInfo RoleCache::create_role(PlayerUid player_uid, yuan::redis::RedisClient *redis, PackedGameServiceId world_service_id) const
    {
        const auto allocated = redis ? redis->incr("game:role:next_id") : nullptr;
        const auto role_id = allocated ? static_cast<RoleId>(std::stoull(allocated->to_string())) : static_cast<RoleId>(player_uid * 100 + 1);
        return SSPlayerRoleInfo{role_id, random_role_name(), 1, world_service_id, 0};
    }

    std::string RoleCache::random_role_name() const
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
}
