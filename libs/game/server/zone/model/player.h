#ifndef YUAN_GAME_SERVER_ZONE_MODEL_PLAYER_H
#define YUAN_GAME_SERVER_ZONE_MODEL_PLAYER_H

#include "common/codec/game_binary_codec.h"

#include <optional>
#include <string>

namespace yuan::game::server
{
    struct Player
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::uint64_t gateway_session_id = 0;
        std::uint32_t level = 1;
        std::uint64_t exp = 0;

        [[nodiscard]] bool valid() const;
        [[nodiscard]] std::string to_json() const;

        static Player from_login(SSGatewayLoginRequest request);
        static std::optional<Player> from_json(const std::string &text);
        static std::optional<Player> from_legacy_text(const std::string &text);
    };
}

#endif
