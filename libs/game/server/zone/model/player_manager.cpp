#include "zone/model/player_manager.h"

#include "internal/def.h"
#include "logger.h"

namespace yuan::game::server
{
    bool PlayerManager::online(SSGatewayLoginRequest request, const PlayerLoader &loader)
    {
        if (request.player_uid == 0 || request.role_id == 0) {
            return false;
        }
        auto data = load_or_create(request, loader);
        data.gateway_session_id = request.gateway_session_id;
        std::scoped_lock lock(mutex_);
        players_by_role_[request.role_id] = data;
        const auto old_session = gateway_session_by_role_.find(request.role_id);
        if (old_session != gateway_session_by_role_.end()) {
            role_by_gateway_session_.erase(old_session->second);
        }
        gateway_session_by_role_[request.role_id] = request.gateway_session_id;
        role_by_gateway_session_[request.gateway_session_id] = request.role_id;
        online_roles_.insert(request.role_id);
        return true;
    }

    bool PlayerManager::offline(SSGatewayLoginRequest request)
    {
        if (request.role_id == 0) {
            return false;
        }
        {
            std::scoped_lock lock(mutex_);
            if (!online_roles_.erase(request.role_id)) {
                return false;
            }
            const auto session_it = gateway_session_by_role_.find(request.role_id);
            if (session_it != gateway_session_by_role_.end()) {
                role_by_gateway_session_.erase(session_it->second);
                gateway_session_by_role_.erase(session_it);
            }
            if (players_by_role_.contains(request.role_id)) {
                dirty_roles_.insert(request.role_id);
            }
        }
        return true;
    }

    bool PlayerManager::set_level(RoleId role_id, std::uint32_t level)
    {
        if (role_id == 0 || level == 0) {
            return false;
        }
        std::scoped_lock lock(mutex_);
        const auto it = players_by_role_.find(role_id);
        if (it == players_by_role_.end() || !online_roles_.contains(role_id)) {
            return false;
        }
        it->second.level = level;
        dirty_roles_.insert(role_id);
        return true;
    }

    void PlayerManager::flush_dirty(const PlayerSaver &saver, PackedGameServiceId zone_service_id)
    {
        if (!saver) {
            LOG_ERROR("zone player flush failed: saver unavailable zone_service={} dirty_roles={}",
                      zone_service_id, dirty_count());
            return;
        }

        std::unordered_map<RoleId, Player> dirty_players;
        {
            std::scoped_lock lock(mutex_);
            for (const auto role_id : dirty_roles_) {
                const auto it = players_by_role_.find(role_id);
                if (it != players_by_role_.end()) {
                    dirty_players.emplace(role_id, it->second);
                }
            }
            dirty_roles_.clear();
        }

        for (const auto &[role_id, data] : dirty_players) {
            if (!saver(data)) {
                LOG_ERROR("zone player flush failed: player_uid={} role_id={} zone_service={}",
                          data.player_uid,
                          data.role_id,
                          zone_service_id);
            }
        }
    }

    std::size_t PlayerManager::dirty_count() const
    {
        std::scoped_lock lock(mutex_);
        return dirty_roles_.size();
    }

    std::size_t PlayerManager::online_count() const
    {
        std::scoped_lock lock(mutex_);
        return online_roles_.size();
    }

    bool PlayerManager::role_online(RoleId role_id) const
    {
        std::scoped_lock lock(mutex_);
        return online_roles_.contains(role_id);
    }

    std::uint64_t PlayerManager::gateway_session_for_role(RoleId role_id) const
    {
        std::scoped_lock lock(mutex_);
        const auto it = gateway_session_by_role_.find(role_id);
        return it == gateway_session_by_role_.end() ? 0 : it->second;
    }

    RoleId PlayerManager::role_for_gateway_session(std::uint64_t gateway_session_id) const
    {
        std::scoped_lock lock(mutex_);
        const auto it = role_by_gateway_session_.find(gateway_session_id);
        return it == role_by_gateway_session_.end() ? 0 : it->second;
    }

    PlayerUid PlayerManager::player_uid_for_role(RoleId role_id) const
    {
        std::scoped_lock lock(mutex_);
        const auto it = players_by_role_.find(role_id);
        return it == players_by_role_.end() ? 0 : it->second.player_uid;
    }

    Player PlayerManager::load_or_create(SSGatewayLoginRequest request, const PlayerLoader &loader) const
    {
        if (loader) {
            if (auto data = loader(request)) {
                return *data;
            }
        }
        return Player::from_login(request);
    }

    void PlayerManager::mark_dirty(RoleId role_id)
    {
        std::scoped_lock lock(mutex_);
        if (players_by_role_.contains(role_id)) {
            dirty_roles_.insert(role_id);
        }
    }
}
