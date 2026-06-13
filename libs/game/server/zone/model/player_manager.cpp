#include "zone/model/player_manager.h"

#include "internal/def.h"
#include "logger.h"
#include "redis_client.h"

namespace yuan::game::server
{
    namespace
    {
        bool redis_set_ok(const std::shared_ptr<yuan::redis::RedisValue> &value)
        {
            return value && value->get_type() != yuan::redis::resp_error && value->to_string() == "OK";
        }
    }

    bool PlayerManager::online(ClientLoginRequest request, yuan::redis::RedisClient *redis)
    {
        if (request.player_uid == 0 || request.role_id == 0) {
            return false;
        }
        auto data = load_or_create(request, redis);
        std::scoped_lock lock(mutex_);
        players_by_role_[request.role_id] = data;
        online_roles_.insert(request.role_id);
        return true;
    }

    bool PlayerManager::offline(ClientLoginRequest request)
    {
        if (request.role_id == 0) {
            return false;
        }
        {
            std::scoped_lock lock(mutex_);
            if (!online_roles_.erase(request.role_id)) {
                return false;
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

    void PlayerManager::flush_dirty(yuan::redis::RedisClient *redis, PackedGameServiceId zone_service_id)
    {
        if (!redis || !redis->ensure_connected()) {
            LOG_ERROR("zone player flush failed: redis unavailable zone_service={} dirty_roles={}",
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
            const auto key = "game:zone:player:" + std::to_string(role_id);
            const auto value = data.to_json();
            const auto saved = redis->set(key, value);
            if (!redis_set_ok(saved)) {
                LOG_ERROR("zone player flush failed: key={} player_uid={} role_id={} redis_result={}",
                          key,
                          data.player_uid,
                          data.role_id,
                          saved ? saved->to_string() : "null");
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

    Player PlayerManager::load_or_create(ClientLoginRequest request, yuan::redis::RedisClient *redis) const
    {
        if (redis && redis->ensure_connected()) {
            const auto value = redis->get("game:zone:player:" + std::to_string(request.role_id));
            if (value && value->get_type() != yuan::redis::resp_null) {
                if (auto data = Player::from_json(value->to_string())) {
                    return *data;
                }
                if (auto data = Player::from_legacy_text(value->to_string())) {
                    return *data;
                }
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
