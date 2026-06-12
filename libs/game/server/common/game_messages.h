#ifndef YUAN_GAME_SERVER_COMMON_GAME_MESSAGES_H
#define YUAN_GAME_SERVER_COMMON_GAME_MESSAGES_H

#include "common/service_node.h"

#include <optional>
#include <string>
#include <vector>

namespace yuan::game::server
{
    using PlayerUid = std::uint64_t;
    using AccountId = PlayerUid;
    using PlayerId = std::uint64_t;
    using RoleId = PlayerId;

    struct GatewayInfo
    {
        PackedGameServiceId service_id = 0;
        std::string host;
        std::uint16_t port = 0;
        std::string name;
    };

    struct PlayerRoleInfo
    {
        RoleId role_id = 0;
        std::string name;
        std::uint32_t level = 1;
        PackedGameServiceId world_service_id = 0;
        PackedGameServiceId zone_service_id = 0;
    };

    struct ZoneInfo
    {
        PackedGameServiceId service_id = 0;
        std::string name;
        std::uint32_t online_players = 0;
        std::uint32_t max_players = 0;
        bool available = true;
    };

    struct LoginOptionsRequest { PlayerUid player_uid = 0; };
    struct LoginOptionsResponse { std::vector<GatewayInfo> gateways; std::vector<PlayerRoleInfo> roles; };
    struct PlayerZoneQuery { PlayerId player_id = 0; };
    struct ZoneSelectRequest { PlayerUid player_uid = 0; RoleId role_id = 0; };
    struct PlayerZoneUpdate { PlayerId player_id = 0; PackedGameServiceId zone_service_id = 0; };
    struct ClientLoginRequest { PlayerUid player_uid = 0; RoleId role_id = 0; };
    struct ClientLoginResponse { bool ok = false; RoleId role_id = 0; PackedGameServiceId zone_service_id = 0; std::string message; };
    struct ClientGameRequest { PlayerUid player_uid = 0; RoleId role_id = 0; yuan::rpc::Bytes payload; };
    struct ClientGameResponse { bool ok = false; RoleId role_id = 0; yuan::rpc::Bytes payload; std::string message; };
    struct ClientTimeSyncRequest { PlayerUid player_uid = 0; RoleId role_id = 0; std::uint64_t client_time_seconds = 0; };
    struct ClientTimeSyncResponse
    {
        bool ok = false;
        RoleId role_id = 0;
        std::uint64_t client_time_seconds = 0;
        std::uint64_t server_receive_time_seconds = 0;
        std::uint64_t server_send_time_seconds = 0;
        std::string message;
    };
    struct WebAuthRequest { std::string account; std::string password; };
    struct WebAuthResponse { bool ok = false; PlayerUid player_uid = 0; LoginOptionsResponse login_options; std::string message; };
    struct GmCommandRequest { PackedGameServiceId target_service_id = 0; std::string command; std::vector<std::string> args; };
    struct GmCommandResponse { bool ok = false; std::string message; };

    bool encode_gateway_info(const GatewayInfo &info, yuan::rpc::Bytes &out);
    bool encode_login_options_request(const LoginOptionsRequest &request, yuan::rpc::Bytes &out);
    bool encode_login_options_response(const LoginOptionsResponse &response, yuan::rpc::Bytes &out);
    bool encode_player_zone_query(const PlayerZoneQuery &query, yuan::rpc::Bytes &out);
    bool encode_player_zone_update(const PlayerZoneUpdate &update, yuan::rpc::Bytes &out);
    bool encode_zone_info(const ZoneInfo &info, yuan::rpc::Bytes &out);
    bool encode_zone_select_request(const ZoneSelectRequest &request, yuan::rpc::Bytes &out);
    bool encode_client_login_request(const ClientLoginRequest &request, yuan::rpc::Bytes &out);
    bool encode_client_login_response(const ClientLoginResponse &response, yuan::rpc::Bytes &out);
    bool encode_client_game_request(const ClientGameRequest &request, yuan::rpc::Bytes &out);
    bool encode_client_game_response(const ClientGameResponse &response, yuan::rpc::Bytes &out);
    bool encode_client_time_sync_request(const ClientTimeSyncRequest &request, yuan::rpc::Bytes &out);
    bool encode_client_time_sync_response(const ClientTimeSyncResponse &response, yuan::rpc::Bytes &out);
    bool encode_web_auth_request(const WebAuthRequest &request, yuan::rpc::Bytes &out);
    bool encode_web_auth_response(const WebAuthResponse &response, yuan::rpc::Bytes &out);
    bool encode_gm_command_request(const GmCommandRequest &request, yuan::rpc::Bytes &out);
    bool encode_gm_command_response(const GmCommandResponse &response, yuan::rpc::Bytes &out);

    std::optional<GatewayInfo> decode_gateway_info(const yuan::rpc::Bytes &in);
    std::optional<LoginOptionsRequest> decode_login_options_request(const yuan::rpc::Bytes &in);
    std::optional<LoginOptionsResponse> decode_login_options_response(const yuan::rpc::Bytes &in);
    std::optional<PlayerZoneQuery> decode_player_zone_query(const yuan::rpc::Bytes &in);
    std::optional<PlayerZoneUpdate> decode_player_zone_update(const yuan::rpc::Bytes &in);
    std::optional<ZoneInfo> decode_zone_info(const yuan::rpc::Bytes &in);
    std::optional<ZoneSelectRequest> decode_zone_select_request(const yuan::rpc::Bytes &in);
    std::optional<ClientLoginRequest> decode_client_login_request(const yuan::rpc::Bytes &in);
    std::optional<ClientLoginResponse> decode_client_login_response(const yuan::rpc::Bytes &in);
    std::optional<ClientGameRequest> decode_client_game_request(const yuan::rpc::Bytes &in);
    std::optional<ClientGameResponse> decode_client_game_response(const yuan::rpc::Bytes &in);
    std::optional<ClientTimeSyncRequest> decode_client_time_sync_request(const yuan::rpc::Bytes &in);
    std::optional<ClientTimeSyncResponse> decode_client_time_sync_response(const yuan::rpc::Bytes &in);
    std::optional<WebAuthRequest> decode_web_auth_request(const yuan::rpc::Bytes &in);
    std::optional<WebAuthResponse> decode_web_auth_response(const yuan::rpc::Bytes &in);
    std::optional<GmCommandRequest> decode_gm_command_request(const yuan::rpc::Bytes &in);
    std::optional<GmCommandResponse> decode_gm_command_response(const yuan::rpc::Bytes &in);
}

#endif
