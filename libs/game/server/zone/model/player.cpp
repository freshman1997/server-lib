#include "zone/model/player.h"

#include <nlohmann/json.hpp>

#include <sstream>

namespace yuan::game::server
{
    bool Player::valid() const
    {
        return player_uid != 0 && role_id != 0;
    }

    std::string Player::to_json() const
    {
        nlohmann::json root;
        root["player_uid"] = player_uid;
        root["role_id"] = role_id;
        root["level"] = level;
        root["exp"] = exp;
        return root.dump();
    }

    Player Player::from_login(SSGatewayLoginRequest request)
    {
        return Player{request.player_uid, request.role_id, request.gateway_session_id, 1, 0};
    }

    std::optional<Player> Player::from_json(const std::string &text)
    {
        try {
            const auto root = nlohmann::json::parse(text);
            Player player;
            player.player_uid = root.value("player_uid", static_cast<PlayerUid>(0));
            player.role_id = root.value("role_id", static_cast<RoleId>(0));
            player.level = root.value("level", static_cast<std::uint32_t>(1));
            player.exp = root.value("exp", static_cast<std::uint64_t>(0));
            if (player.valid()) {
                return player;
            }
        } catch (...) {
        }
        return std::nullopt;
    }

    std::optional<Player> Player::from_legacy_text(const std::string &text)
    {
        std::stringstream stream(text);
        std::string field;
        Player player;
        if (std::getline(stream, field, '\n')) {
            player.player_uid = static_cast<PlayerUid>(std::stoull(field));
        }

        if (std::getline(stream, field, '\n')) {
            player.role_id = static_cast<RoleId>(std::stoull(field));
        }

        if (std::getline(stream, field, '\n')) {
            player.level = static_cast<std::uint32_t>(std::stoul(field));
        }

        if (std::getline(stream, field, '\n')) {
            player.exp = static_cast<std::uint64_t>(std::stoull(field));
        }
        
        if (player.valid()) {
            return player;
        }
        return std::nullopt;
    }
}
