#include "world/model/role.h"

#include "logger.h"

#include <algorithm>

namespace yuan::game::server
{
    bool RoleCache::is_player_loaded(PlayerUid player_uid) const
    {
        std::scoped_lock lock(mutex_);
        return loaded_players_.contains(player_uid);
    }

    void RoleCache::apply_player_roles(const LoadedPlayerRoles &loaded,
                                       std::function<void(PlayerUid, const SSPlayerRoleInfo &)> add_role)
    {
        if (loaded.player_uid == 0 || loaded.roles.empty()) {
            return;
        }

        std::scoped_lock lock(mutex_);
        if (loaded_players_.contains(loaded.player_uid)) {
            return;
        }
        auto &role_ids = role_ids_by_player_uid_[loaded.player_uid];
        for (const auto &role : loaded.roles) {
            role_ids.push_back(role.role_id);
            roles_by_id_[role.role_id] = role;
            add_role(loaded.player_uid, role);
        }
        if (loaded.created_default_role || loaded.missing_role_list) {
            dirty_players_.insert(loaded.player_uid);
            for (const auto &role : loaded.roles) {
                dirty_roles_.insert(role.role_id);
            }
        }
        loaded_players_.insert(loaded.player_uid);
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

    std::size_t RoleCache::flush_dirty(const PlayerRolesSaver &saver, PackedGameServiceId world_service_id)
    {
        if (!saver) {
            LOG_ERROR("world role flush failed: saver unavailable world_service={} dirty_players={} dirty_roles={}",
                      world_service_id, dirty_player_count(), dirty_role_count());
            return dirty_player_count() + dirty_role_count();
        }

        std::unordered_set<PlayerUid> dirty_players;
        std::unordered_set<RoleId> dirty_roles;
        std::unordered_map<PlayerUid, std::vector<RoleId>> role_ids_by_player_uid;
        std::unordered_map<RoleId, SSPlayerRoleInfo> roles_by_id;
        {
            std::scoped_lock lock(mutex_);
            dirty_players = dirty_players_;
            dirty_roles = dirty_roles_;
            role_ids_by_player_uid = role_ids_by_player_uid_;
            roles_by_id = roles_by_id_;
        }

        for (const auto role_id : dirty_roles) {
            const auto role_it = roles_by_id.find(role_id);
            if (role_it == roles_by_id.end()) {
                continue;
            }
            for (const auto &[player_uid, role_ids] : role_ids_by_player_uid) {
                if (std::find(role_ids.begin(), role_ids.end(), role_id) != role_ids.end()) {
                    dirty_players.insert(player_uid);
                    break;
                }
            }
        }

        for (const auto player_uid : dirty_players) {
            const auto it = role_ids_by_player_uid.find(player_uid);
            if (it == role_ids_by_player_uid.end()) {
                continue;
            }
            std::vector<SSPlayerRoleInfo> roles;
            roles.reserve(it->second.size());
            for (const auto role_id : it->second) {
                const auto role_it = roles_by_id.find(role_id);
                if (role_it != roles_by_id.end()) {
                    roles.push_back(role_it->second);
                }
            }
            if (saver(player_uid, roles)) {
                std::scoped_lock lock(mutex_);
                dirty_players_.erase(player_uid);
                for (const auto &role : roles) {
                    dirty_roles_.erase(role.role_id);
                }
            } else {
                LOG_ERROR("world role flush failed player_uid={} world_service={}", player_uid, world_service_id);
            }
        }
        return dirty_player_count() + dirty_role_count();
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
}
