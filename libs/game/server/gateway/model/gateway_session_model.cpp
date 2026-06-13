#include "gateway/model/gateway_session_model.h"

namespace yuan::game::server
{
    std::uint64_t GatewaySessionModel::login_role(RoleId role_id, PackedGameServiceId zone_service_id)
    {
        return login_role(role_id, zone_service_id, 0);
    }

    std::uint64_t GatewaySessionModel::login_role(RoleId role_id, PackedGameServiceId zone_service_id, std::uint64_t connection_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (role_id != 0 && zone_service_id != 0) {
            logout_role_locked(role_id);
            const auto session_id = next_session_id_++;
            zone_by_role_[role_id] = zone_service_id;
            session_by_role_[role_id] = session_id;
            session_info_by_id_[session_id] = SessionInfo{role_id, zone_service_id, connection_id};
            return session_id;
        }
        return 0;
    }

    void GatewaySessionModel::logout_role(RoleId role_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        logout_role_locked(role_id);
    }

    void GatewaySessionModel::logout_role_locked(RoleId role_id)
    {
        const auto session_it = session_by_role_.find(role_id);
        if (session_it != session_by_role_.end()) {
            session_info_by_id_.erase(session_it->second);
        }
        zone_by_role_.erase(role_id);
        session_by_role_.erase(role_id);
    }

    void GatewaySessionModel::logout_session(std::uint64_t gateway_session_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = session_info_by_id_.find(gateway_session_id);
        if (it == session_info_by_id_.end()) {
            return;
        }
        const auto role_id = it->second.role_id;
        session_info_by_id_.erase(it);
        zone_by_role_.erase(role_id);
        session_by_role_.erase(role_id);
    }

    PackedGameServiceId GatewaySessionModel::zone_for_role(RoleId role_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = zone_by_role_.find(role_id);
        return it == zone_by_role_.end() ? 0 : it->second;
    }

    PackedGameServiceId GatewaySessionModel::zone_for_session(std::uint64_t gateway_session_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = session_info_by_id_.find(gateway_session_id);
        return it == session_info_by_id_.end() ? 0 : it->second.zone_service_id;
    }

    std::uint64_t GatewaySessionModel::session_for_role(RoleId role_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = session_by_role_.find(role_id);
        return it == session_by_role_.end() ? 0 : it->second;
    }

    std::uint64_t GatewaySessionModel::connection_for_session(std::uint64_t gateway_session_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = session_info_by_id_.find(gateway_session_id);
        return it == session_info_by_id_.end() ? 0 : it->second.connection_id;
    }

    std::optional<GatewaySessionModel::SessionInfo> GatewaySessionModel::session_info(std::uint64_t gateway_session_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = session_info_by_id_.find(gateway_session_id);
        if (it == session_info_by_id_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::vector<GatewaySessionModel::SessionRecord> GatewaySessionModel::sessions_for_connection(std::uint64_t connection_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<SessionRecord> sessions;
        for (const auto &[session_id, info] : session_info_by_id_) {
            if (info.connection_id == connection_id) {
                sessions.push_back(SessionRecord{session_id, info.role_id, info.zone_service_id, info.connection_id});
            }
        }
        return sessions;
    }

    std::size_t GatewaySessionModel::session_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return session_info_by_id_.size();
    }

    bool GatewaySessionModel::validate_role_session(RoleId role_id, std::uint64_t gateway_session_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = session_by_role_.find(role_id);
        return gateway_session_id != 0 && it != session_by_role_.end() && it->second == gateway_session_id;
    }

    bool GatewaySessionModel::role_logged_in(RoleId role_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return zone_by_role_.find(role_id) != zone_by_role_.end();
    }
}
