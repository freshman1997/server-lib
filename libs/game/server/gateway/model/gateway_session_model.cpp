#include "gateway/model/gateway_session_model.h"

namespace yuan::game::server
{
    std::uint64_t GatewaySessionModel::reserve_connection(std::uint64_t connection_id, PackedGameServiceId zone_service_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connection_id == 0 || zone_service_id == 0) {
            return 0;
        }

        const auto existing = session_by_connection_.find(connection_id);
        if (existing != session_by_connection_.end()) {
            session_info_by_id_.erase(existing->second);
            session_by_connection_.erase(existing);
        }

        const auto session_id = next_session_id_++;
        session_info_by_id_[session_id] = SessionInfo{zone_service_id, connection_id};
        session_by_connection_[connection_id] = session_id;
        
        return session_id;
    }

    bool GatewaySessionModel::bind_session(std::uint64_t gateway_session_id, PackedGameServiceId zone_service_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = session_info_by_id_.find(gateway_session_id);
        if (it == session_info_by_id_.end() || zone_service_id == 0) {
            return false;
        }
        it->second.zone_service_id = zone_service_id;
        if (it->second.connection_id != 0) {
            session_by_connection_[it->second.connection_id] = gateway_session_id;
        }
        return true;
    }

    void GatewaySessionModel::logout_session(std::uint64_t gateway_session_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = session_info_by_id_.find(gateway_session_id);
        if (it == session_info_by_id_.end()) {
            return;
        }
        session_by_connection_.erase(it->second.connection_id);
        session_info_by_id_.erase(it);
    }

    PackedGameServiceId GatewaySessionModel::zone_for_session(std::uint64_t gateway_session_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = session_info_by_id_.find(gateway_session_id);
        return it == session_info_by_id_.end() ? 0 : it->second.zone_service_id;
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

    std::optional<GatewaySessionModel::SessionRecord> GatewaySessionModel::session_for_connection(std::uint64_t connection_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto session_it = session_by_connection_.find(connection_id);
        if (session_it == session_by_connection_.end()) {
            return std::nullopt;
        }
        const auto info_it = session_info_by_id_.find(session_it->second);
        if (info_it == session_info_by_id_.end()) {
            return std::nullopt;
        }
        return SessionRecord{session_it->second, info_it->second.zone_service_id, info_it->second.connection_id};
    }

    std::vector<GatewaySessionModel::SessionRecord> GatewaySessionModel::sessions_for_connection(std::uint64_t connection_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<SessionRecord> sessions;
        for (const auto &[session_id, info] : session_info_by_id_) {
            if (info.connection_id == connection_id) {
                sessions.push_back(SessionRecord{session_id, info.zone_service_id, info.connection_id});
            }
        }
        return sessions;
    }

    std::vector<GatewaySessionModel::SessionRecord> GatewaySessionModel::all_sessions() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<SessionRecord> sessions;
        sessions.reserve(session_info_by_id_.size());
        for (const auto &[session_id, info] : session_info_by_id_) {
            sessions.push_back(SessionRecord{session_id, info.zone_service_id, info.connection_id});
        }
        return sessions;
    }

    std::size_t GatewaySessionModel::session_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return session_info_by_id_.size();
    }

}
