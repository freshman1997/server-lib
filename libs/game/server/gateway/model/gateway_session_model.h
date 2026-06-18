#ifndef YUAN_GAME_SERVER_GATEWAY_MODEL_GATEWAY_SESSION_MODEL_H
#define YUAN_GAME_SERVER_GATEWAY_MODEL_GATEWAY_SESSION_MODEL_H

#include "common/codec/game_binary_codec.h"

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
            PackedGameServiceId zone_service_id = 0;
            std::uint64_t connection_id = 0;
        };

        struct SessionRecord
        {
            std::uint64_t gateway_session_id = 0;
            PackedGameServiceId zone_service_id = 0;
            std::uint64_t connection_id = 0;
        };

        [[nodiscard]] std::uint64_t reserve_connection(std::uint64_t connection_id, PackedGameServiceId zone_service_id);
        [[nodiscard]] bool bind_session(std::uint64_t gateway_session_id, PackedGameServiceId zone_service_id);
        void logout_session(std::uint64_t gateway_session_id);
        [[nodiscard]] PackedGameServiceId zone_for_session(std::uint64_t gateway_session_id) const;
        [[nodiscard]] std::uint64_t connection_for_session(std::uint64_t gateway_session_id) const;
        [[nodiscard]] std::optional<SessionInfo> session_info(std::uint64_t gateway_session_id) const;
        [[nodiscard]] std::optional<SessionRecord> session_for_connection(std::uint64_t connection_id) const;
        [[nodiscard]] std::vector<SessionRecord> sessions_for_connection(std::uint64_t connection_id) const;
        [[nodiscard]] std::vector<SessionRecord> all_sessions() const;
        [[nodiscard]] std::size_t session_count() const;

    private:
        mutable std::mutex mutex_;
        std::uint64_t next_session_id_ = 1;
        std::unordered_map<std::uint64_t, std::uint64_t> session_by_connection_;
        std::unordered_map<std::uint64_t, SessionInfo> session_info_by_id_;
    };
}

#endif
