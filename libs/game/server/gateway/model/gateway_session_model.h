#ifndef YUAN_GAME_SERVER_GATEWAY_MODEL_GATEWAY_SESSION_MODEL_H
#define YUAN_GAME_SERVER_GATEWAY_MODEL_GATEWAY_SESSION_MODEL_H

#include "common/game_messages.h"

#include <optional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace yuan::game::server
{
    class GatewaySessionModel
    {
    public:
        struct SessionInfo
        {
            RoleId role_id = 0;
            PackedGameServiceId zone_service_id = 0;
            std::uint64_t connection_id = 0;
        };

        struct SessionRecord
        {
            std::uint64_t gateway_session_id = 0;
            RoleId role_id = 0;
            PackedGameServiceId zone_service_id = 0;
            std::uint64_t connection_id = 0;
        };

        [[nodiscard]] std::uint64_t login_role(RoleId role_id, PackedGameServiceId zone_service_id);
        [[nodiscard]] std::uint64_t login_role(RoleId role_id, PackedGameServiceId zone_service_id, std::uint64_t connection_id);
        void logout_role(RoleId role_id);
        void logout_session(std::uint64_t gateway_session_id);
        [[nodiscard]] PackedGameServiceId zone_for_role(RoleId role_id) const;
        [[nodiscard]] PackedGameServiceId zone_for_session(std::uint64_t gateway_session_id) const;
        [[nodiscard]] std::uint64_t session_for_role(RoleId role_id) const;
        [[nodiscard]] std::uint64_t connection_for_session(std::uint64_t gateway_session_id) const;
        [[nodiscard]] std::optional<SessionInfo> session_info(std::uint64_t gateway_session_id) const;
        [[nodiscard]] std::vector<SessionRecord> sessions_for_connection(std::uint64_t connection_id) const;
        [[nodiscard]] std::size_t session_count() const;
        [[nodiscard]] bool validate_role_session(RoleId role_id, std::uint64_t gateway_session_id) const;
        [[nodiscard]] bool role_logged_in(RoleId role_id) const;

    private:
        void logout_role_locked(RoleId role_id);

        mutable std::mutex mutex_;
        std::uint64_t next_session_id_ = 1;
        std::unordered_map<RoleId, PackedGameServiceId> zone_by_role_;
        std::unordered_map<RoleId, std::uint64_t> session_by_role_;
        std::unordered_map<std::uint64_t, SessionInfo> session_info_by_id_;
    };
}

#endif
