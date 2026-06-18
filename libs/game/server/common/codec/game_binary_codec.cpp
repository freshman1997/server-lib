#include "common/codec/game_binary_codec.h"
#include "common/codec/binary_codec.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace yuan::game::server
{
    namespace
    {
        using binary_codec::Reader;
        using binary_codec::Writer;
    }

    std::string encode_login_options_response_json(const LoginOptionsResponse &response)
    {
        nlohmann::json root;
        root["gateways"] = nlohmann::json::array();
        for (const auto &gateway : response.gateways) {
            root["gateways"].push_back({{"host", gateway.host},
                                         {"port", gateway.port},
                                         {"name", gateway.name}});
        }
        root["roles"] = nlohmann::json::array();
        for (const auto &role : response.roles) {
            root["roles"].push_back({{"role_id", role.role_id},
                                      {"name", role.name},
                                      {"level", role.level},
                                      {"login_token_id", role.login_token_id}});
        }
        root["zones"] = nlohmann::json::array();
        for (const auto &zone : response.zones) {
            root["zones"].push_back({{"name", zone.name},
                                      {"online_players", zone.online_players},
                                      {"max_players", zone.max_players},
                                      {"available", zone.available}});
        }
        return root.dump();
    }

    std::optional<LoginOptionsResponse> decode_login_options_response_json(const std::string &json_text)
    {
        try {
            const auto root = nlohmann::json::parse(json_text);
            LoginOptionsResponse response;
            for (const auto &item : root.value("gateways", nlohmann::json::array())) {
                SSGatewayInfo gateway;
                gateway.service_id = item.value("service_id", static_cast<PackedGameServiceId>(0));
                gateway.host = item.value("host", std::string());
                gateway.port = item.value("port", static_cast<std::uint16_t>(0));
                gateway.name = item.value("name", std::string());
                response.gateways.push_back(std::move(gateway));
            }
            for (const auto &item : root.value("roles", nlohmann::json::array())) {
                SSPlayerRoleInfo role;
                role.role_id = item.value("role_id", static_cast<RoleId>(0));
                role.name = item.value("name", std::string());
                role.level = item.value("level", static_cast<std::uint32_t>(1));
                role.world_service_id = item.value("world_service_id", static_cast<PackedGameServiceId>(0));
                role.zone_service_id = item.value("zone_service_id", static_cast<PackedGameServiceId>(0));
                role.login_token_id = item.value("login_token_id", LoginTokenId{});
                response.roles.push_back(std::move(role));
            }
            for (const auto &item : root.value("zones", nlohmann::json::array())) {
                SSZoneInfo zone;
                zone.service_id = item.value("service_id", static_cast<PackedGameServiceId>(0));
                zone.name = item.value("name", std::string());
                zone.online_players = item.value("online_players", static_cast<std::uint32_t>(0));
                zone.max_players = item.value("max_players", static_cast<std::uint32_t>(0));
                zone.available = item.value("available", true);
                response.zones.push_back(std::move(zone));
            }
            return response;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<SSPlayerZoneUpdate> decode_player_zone_update(const yuan::rpc::Bytes &in)
    {
        SSPlayerZoneUpdate update;
        Reader reader(in);
        if (!binary_codec::read_version(reader) || !reader.fields(update.player_uid, update.player_id, update.zone_service_id)) {
            return std::nullopt;
        }
        if (!reader.done() && !reader.fields(update.source_zone_service_id)) {
            return std::nullopt;
        }
        if (!reader.done() && !reader.fields(update.gateway_session_id)) {
            return std::nullopt;
        }
        if (!reader.done()) {
            return std::nullopt;
        }
        return update;
    }

}
