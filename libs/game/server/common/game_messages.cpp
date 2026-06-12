#include "common/game_messages.h"

#include <cstring>
#include <limits>
#include <utility>

namespace yuan::game::server
{
    namespace
    {
        void append_u16(yuan::rpc::Bytes &out, std::uint16_t value)
        {
            out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
            out.push_back(static_cast<std::uint8_t>(value & 0xff));
        }

        void append_u32(yuan::rpc::Bytes &out, std::uint32_t value)
        {
            out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
            out.push_back(static_cast<std::uint8_t>(value & 0xff));
        }

        void append_u64(yuan::rpc::Bytes &out, std::uint64_t value)
        {
            for (int shift = 56; shift >= 0; shift -= 8) {
                out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
            }
        }

        bool read_u16(const yuan::rpc::Bytes &in, std::size_t &offset, std::uint16_t &value)
        {
            if (in.size() - offset < sizeof(std::uint16_t)) {
                return false;
            }
            value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[offset]) << 8) | in[offset + 1]);
            offset += sizeof(std::uint16_t);
            return true;
        }

        bool read_u32(const yuan::rpc::Bytes &in, std::size_t &offset, std::uint32_t &value)
        {
            if (in.size() - offset < sizeof(std::uint32_t)) {
                return false;
            }
            value = (static_cast<std::uint32_t>(in[offset]) << 24) |
                    (static_cast<std::uint32_t>(in[offset + 1]) << 16) |
                    (static_cast<std::uint32_t>(in[offset + 2]) << 8) |
                    static_cast<std::uint32_t>(in[offset + 3]);
            offset += sizeof(std::uint32_t);
            return true;
        }

        bool read_u64(const yuan::rpc::Bytes &in, std::size_t &offset, std::uint64_t &value)
        {
            if (in.size() - offset < sizeof(std::uint64_t)) {
                return false;
            }
            value = 0;
            for (int i = 0; i < 8; ++i) {
                value = (value << 8) | in[offset + static_cast<std::size_t>(i)];
            }
            offset += sizeof(std::uint64_t);
            return true;
        }

        bool append_string(yuan::rpc::Bytes &out, const std::string &value)
        {
            if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }
            append_u32(out, static_cast<std::uint32_t>(value.size()));
            const auto offset = out.size();
            out.resize(offset + value.size());
            if (!value.empty()) {
                std::memcpy(out.data() + offset, value.data(), value.size());
            }
            return true;
        }

        bool read_string(const yuan::rpc::Bytes &in, std::size_t &offset, std::string &value)
        {
            std::uint32_t size = 0;
            if (!read_u32(in, offset, size) || in.size() - offset < size) {
                return false;
            }
            value.assign(reinterpret_cast<const char *>(in.data() + offset), size);
            offset += size;
            return true;
        }

        bool append_bytes(yuan::rpc::Bytes &out, const yuan::rpc::Bytes &value)
        {
            if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }
            append_u32(out, static_cast<std::uint32_t>(value.size()));
            out.insert(out.end(), value.begin(), value.end());
            return true;
        }

        bool read_bytes(const yuan::rpc::Bytes &in, std::size_t &offset, yuan::rpc::Bytes &value)
        {
            std::uint32_t size = 0;
            if (!read_u32(in, offset, size) || in.size() - offset < size) {
                return false;
            }
            value.assign(in.begin() + static_cast<std::ptrdiff_t>(offset), in.begin() + static_cast<std::ptrdiff_t>(offset + size));
            offset += size;
            return true;
        }

        void encode_gateway_info_body(const GatewayInfo &info, yuan::rpc::Bytes &out)
        {
            append_u64(out, info.service_id);
            (void)append_string(out, info.host);
            append_u16(out, info.port);
            (void)append_string(out, info.name);
        }

        bool decode_gateway_info_body(const yuan::rpc::Bytes &in, std::size_t &offset, GatewayInfo &info)
        {
            return read_u64(in, offset, info.service_id) && read_string(in, offset, info.host) && read_u16(in, offset, info.port) && read_string(in, offset, info.name);
        }

        void encode_role_body(const PlayerRoleInfo &role, yuan::rpc::Bytes &out)
        {
            append_u64(out, role.role_id);
            (void)append_string(out, role.name);
            append_u32(out, role.level);
            append_u64(out, role.world_service_id);
            append_u64(out, role.zone_service_id);
        }

        bool decode_role_body(const yuan::rpc::Bytes &in, std::size_t &offset, PlayerRoleInfo &role)
        {
            return read_u64(in, offset, role.role_id) && read_string(in, offset, role.name) && read_u32(in, offset, role.level) &&
                   read_u64(in, offset, role.world_service_id) && read_u64(in, offset, role.zone_service_id);
        }

        void encode_zone_info_body(const ZoneInfo &info, yuan::rpc::Bytes &out)
        {
            append_u64(out, info.service_id);
            (void)append_string(out, info.name);
            append_u32(out, info.online_players);
            append_u32(out, info.max_players);
            out.push_back(info.available ? 1 : 0);
        }

        bool decode_zone_info_body(const yuan::rpc::Bytes &in, std::size_t &offset, ZoneInfo &info)
        {
            if (!read_u64(in, offset, info.service_id) || !read_string(in, offset, info.name) ||
                !read_u32(in, offset, info.online_players) || !read_u32(in, offset, info.max_players) || in.size() - offset < 1) {
                return false;
            }
            info.available = in[offset++] != 0;
            return true;
        }
    }

    bool encode_gateway_info(const GatewayInfo &info, yuan::rpc::Bytes &out)
    {
        out.clear();
        append_u32(out, 1);
        encode_gateway_info_body(info, out);
        return true;
    }

    bool encode_login_options_request(const LoginOptionsRequest &request, yuan::rpc::Bytes &out)
    {
        out.clear();
        append_u32(out, 1);
        append_u64(out, request.player_uid);
        return true;
    }

    bool encode_login_options_response(const LoginOptionsResponse &response, yuan::rpc::Bytes &out)
    {
        if (response.gateways.size() > std::numeric_limits<std::uint32_t>::max() || response.roles.size() > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        out.clear();
        append_u32(out, 1);
        append_u32(out, static_cast<std::uint32_t>(response.gateways.size()));
        for (const auto &gateway : response.gateways) {
            encode_gateway_info_body(gateway, out);
        }
        append_u32(out, static_cast<std::uint32_t>(response.roles.size()));
        for (const auto &role : response.roles) {
            encode_role_body(role, out);
        }
        return true;
    }

    bool encode_player_zone_query(const PlayerZoneQuery &query, yuan::rpc::Bytes &out)
    {
        out.clear();
        append_u32(out, 1);
        append_u64(out, query.player_id);
        return true;
    }

    bool encode_player_zone_update(const PlayerZoneUpdate &update, yuan::rpc::Bytes &out)
    {
        out.clear();
        append_u32(out, 1);
        append_u64(out, update.player_id);
        append_u64(out, update.zone_service_id);
        return true;
    }

    bool encode_zone_info(const ZoneInfo &info, yuan::rpc::Bytes &out)
    {
        out.clear();
        append_u32(out, 1);
        encode_zone_info_body(info, out);
        return true;
    }

    bool encode_zone_select_request(const ZoneSelectRequest &request, yuan::rpc::Bytes &out)
    {
        out.clear();
        append_u32(out, 1);
        append_u64(out, request.player_uid);
        append_u64(out, request.role_id);
        return true;
    }

    bool encode_client_login_request(const ClientLoginRequest &request, yuan::rpc::Bytes &out)
    {
        out.clear();
        append_u32(out, 1);
        append_u64(out, request.player_uid);
        append_u64(out, request.role_id);
        return true;
    }

    bool encode_client_login_response(const ClientLoginResponse &response, yuan::rpc::Bytes &out)
    {
        out.clear();
        append_u32(out, 1);
        out.push_back(response.ok ? 1 : 0);
        append_u64(out, response.role_id);
        append_u64(out, response.zone_service_id);
        return append_string(out, response.message);
    }

    bool encode_client_game_request(const ClientGameRequest &request, yuan::rpc::Bytes &out)
    {
        out.clear();
        append_u32(out, 1);
        append_u64(out, request.player_uid);
        append_u64(out, request.role_id);
        return append_bytes(out, request.payload);
    }

    bool encode_client_game_response(const ClientGameResponse &response, yuan::rpc::Bytes &out)
    {
        out.clear();
        append_u32(out, 1);
        out.push_back(response.ok ? 1 : 0);
        append_u64(out, response.role_id);
        return append_bytes(out, response.payload) && append_string(out, response.message);
    }

    bool encode_client_time_sync_request(const ClientTimeSyncRequest &request, yuan::rpc::Bytes &out)
    {
        out.clear();
        append_u32(out, 1);
        append_u64(out, request.player_uid);
        append_u64(out, request.role_id);
        append_u64(out, request.client_time_seconds);
        return true;
    }

    bool encode_client_time_sync_response(const ClientTimeSyncResponse &response, yuan::rpc::Bytes &out)
    {
        out.clear();
        append_u32(out, 1);
        out.push_back(response.ok ? 1 : 0);
        append_u64(out, response.role_id);
        append_u64(out, response.client_time_seconds);
        append_u64(out, response.server_receive_time_seconds);
        append_u64(out, response.server_send_time_seconds);
        return append_string(out, response.message);
    }

    bool encode_web_auth_request(const WebAuthRequest &request, yuan::rpc::Bytes &out)
    {
        out.clear();
        append_u32(out, 1);
        return append_string(out, request.account) && append_string(out, request.password);
    }

    bool encode_web_auth_response(const WebAuthResponse &response, yuan::rpc::Bytes &out)
    {
        yuan::rpc::Bytes options;
        if (!encode_login_options_response(response.login_options, options)) {
            return false;
        }
        out.clear();
        append_u32(out, 1);
        out.push_back(response.ok ? 1 : 0);
        append_u64(out, response.player_uid);
        return append_bytes(out, options) && append_string(out, response.message);
    }

    bool encode_gm_command_request(const GmCommandRequest &request, yuan::rpc::Bytes &out)
    {
        if (request.args.size() > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        out.clear();
        append_u32(out, 1);
        append_u64(out, request.target_service_id);
        if (!append_string(out, request.command)) {
            return false;
        }
        append_u32(out, static_cast<std::uint32_t>(request.args.size()));
        for (const auto &arg : request.args) {
            if (!append_string(out, arg)) {
                return false;
            }
        }
        return true;
    }

    bool encode_gm_command_response(const GmCommandResponse &response, yuan::rpc::Bytes &out)
    {
        out.clear();
        append_u32(out, 1);
        out.push_back(response.ok ? 1 : 0);
        return append_string(out, response.message);
    }

    std::optional<GatewayInfo> decode_gateway_info(const yuan::rpc::Bytes &in)
    {
        GatewayInfo info;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        if (!read_u32(in, offset, version) || version != 1 || !decode_gateway_info_body(in, offset, info) || offset != in.size()) {
            return std::nullopt;
        }
        return info;
    }

    std::optional<LoginOptionsRequest> decode_login_options_request(const yuan::rpc::Bytes &in)
    {
        LoginOptionsRequest request;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        if (!read_u32(in, offset, version) || version != 1 || !read_u64(in, offset, request.player_uid) || offset != in.size()) {
            return std::nullopt;
        }
        return request;
    }

    std::optional<LoginOptionsResponse> decode_login_options_response(const yuan::rpc::Bytes &in)
    {
        LoginOptionsResponse response;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        std::uint32_t gateway_count = 0;
        std::uint32_t role_count = 0;
        if (!read_u32(in, offset, version) || version != 1 || !read_u32(in, offset, gateway_count)) {
            return std::nullopt;
        }
        response.gateways.resize(gateway_count);
        for (auto &gateway : response.gateways) {
            if (!decode_gateway_info_body(in, offset, gateway)) {
                return std::nullopt;
            }
        }
        if (!read_u32(in, offset, role_count)) {
            return std::nullopt;
        }
        response.roles.resize(role_count);
        for (auto &role : response.roles) {
            if (!decode_role_body(in, offset, role)) {
                return std::nullopt;
            }
        }
        if (offset != in.size()) {
            return std::nullopt;
        }
        return response;
    }

    std::optional<PlayerZoneQuery> decode_player_zone_query(const yuan::rpc::Bytes &in)
    {
        PlayerZoneQuery query;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        if (!read_u32(in, offset, version) || version != 1 || !read_u64(in, offset, query.player_id) || offset != in.size()) {
            return std::nullopt;
        }
        return query;
    }

    std::optional<PlayerZoneUpdate> decode_player_zone_update(const yuan::rpc::Bytes &in)
    {
        PlayerZoneUpdate update;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        if (!read_u32(in, offset, version) || version != 1 || !read_u64(in, offset, update.player_id) || !read_u64(in, offset, update.zone_service_id) || offset != in.size()) {
            return std::nullopt;
        }
        return update;
    }

    std::optional<ZoneInfo> decode_zone_info(const yuan::rpc::Bytes &in)
    {
        ZoneInfo info;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        if (!read_u32(in, offset, version) || version != 1 || !decode_zone_info_body(in, offset, info) || offset != in.size()) {
            return std::nullopt;
        }
        return info;
    }

    std::optional<ZoneSelectRequest> decode_zone_select_request(const yuan::rpc::Bytes &in)
    {
        ZoneSelectRequest request;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        if (!read_u32(in, offset, version) || version != 1 || !read_u64(in, offset, request.player_uid) || !read_u64(in, offset, request.role_id) || offset != in.size()) {
            return std::nullopt;
        }
        return request;
    }

    std::optional<ClientLoginRequest> decode_client_login_request(const yuan::rpc::Bytes &in)
    {
        ClientLoginRequest request;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        if (!read_u32(in, offset, version) || version != 1 || !read_u64(in, offset, request.player_uid) || !read_u64(in, offset, request.role_id) || offset != in.size()) {
            return std::nullopt;
        }
        return request;
    }

    std::optional<ClientLoginResponse> decode_client_login_response(const yuan::rpc::Bytes &in)
    {
        ClientLoginResponse response;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        if (!read_u32(in, offset, version) || version != 1 || in.size() - offset < 1) {
            return std::nullopt;
        }
        response.ok = in[offset++] != 0;
        if (!read_u64(in, offset, response.role_id) || !read_u64(in, offset, response.zone_service_id) || !read_string(in, offset, response.message) || offset != in.size()) {
            return std::nullopt;
        }
        return response;
    }

    std::optional<ClientGameRequest> decode_client_game_request(const yuan::rpc::Bytes &in)
    {
        ClientGameRequest request;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        if (!read_u32(in, offset, version) || version != 1 || !read_u64(in, offset, request.player_uid) || !read_u64(in, offset, request.role_id) ||
            !read_bytes(in, offset, request.payload) || offset != in.size()) {
            return std::nullopt;
        }
        return request;
    }

    std::optional<ClientGameResponse> decode_client_game_response(const yuan::rpc::Bytes &in)
    {
        ClientGameResponse response;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        if (!read_u32(in, offset, version) || version != 1 || in.size() - offset < 1) {
            return std::nullopt;
        }
        response.ok = in[offset++] != 0;
        if (!read_u64(in, offset, response.role_id) || !read_bytes(in, offset, response.payload) || !read_string(in, offset, response.message) || offset != in.size()) {
            return std::nullopt;
        }
        return response;
    }

    std::optional<ClientTimeSyncRequest> decode_client_time_sync_request(const yuan::rpc::Bytes &in)
    {
        ClientTimeSyncRequest request;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        if (!read_u32(in, offset, version) || version != 1 || !read_u64(in, offset, request.player_uid) ||
            !read_u64(in, offset, request.role_id) || !read_u64(in, offset, request.client_time_seconds) || offset != in.size()) {
            return std::nullopt;
        }
        return request;
    }

    std::optional<ClientTimeSyncResponse> decode_client_time_sync_response(const yuan::rpc::Bytes &in)
    {
        ClientTimeSyncResponse response;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        if (!read_u32(in, offset, version) || version != 1 || in.size() - offset < 1) {
            return std::nullopt;
        }
        response.ok = in[offset++] != 0;
        if (!read_u64(in, offset, response.role_id) || !read_u64(in, offset, response.client_time_seconds) ||
            !read_u64(in, offset, response.server_receive_time_seconds) || !read_u64(in, offset, response.server_send_time_seconds) ||
            !read_string(in, offset, response.message) || offset != in.size()) {
            return std::nullopt;
        }
        return response;
    }

    std::optional<WebAuthRequest> decode_web_auth_request(const yuan::rpc::Bytes &in)
    {
        WebAuthRequest request;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        if (!read_u32(in, offset, version) || version != 1 || !read_string(in, offset, request.account) || !read_string(in, offset, request.password) || offset != in.size()) {
            return std::nullopt;
        }
        return request;
    }

    std::optional<WebAuthResponse> decode_web_auth_response(const yuan::rpc::Bytes &in)
    {
        WebAuthResponse response;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        if (!read_u32(in, offset, version) || version != 1 || in.size() - offset < 1) {
            return std::nullopt;
        }
        response.ok = in[offset++] != 0;
        yuan::rpc::Bytes options;
        if (!read_u64(in, offset, response.player_uid) || !read_bytes(in, offset, options) || !read_string(in, offset, response.message) || offset != in.size()) {
            return std::nullopt;
        }
        const auto login_options = decode_login_options_response(options);
        if (!login_options) {
            return std::nullopt;
        }
        response.login_options = *login_options;
        return response;
    }

    std::optional<GmCommandRequest> decode_gm_command_request(const yuan::rpc::Bytes &in)
    {
        GmCommandRequest request;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        std::uint32_t arg_count = 0;
        if (!read_u32(in, offset, version) || version != 1 || !read_u64(in, offset, request.target_service_id) ||
            !read_string(in, offset, request.command) || !read_u32(in, offset, arg_count)) {
            return std::nullopt;
        }
        request.args.resize(arg_count);
        for (auto &arg : request.args) {
            if (!read_string(in, offset, arg)) {
                return std::nullopt;
            }
        }
        if (offset != in.size()) {
            return std::nullopt;
        }
        return request;
    }

    std::optional<GmCommandResponse> decode_gm_command_response(const yuan::rpc::Bytes &in)
    {
        GmCommandResponse response;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        if (!read_u32(in, offset, version) || version != 1 || in.size() - offset < 1) {
            return std::nullopt;
        }
        response.ok = in[offset++] != 0;
        if (!read_string(in, offset, response.message) || offset != in.size()) {
            return std::nullopt;
        }
        return response;
    }
}
