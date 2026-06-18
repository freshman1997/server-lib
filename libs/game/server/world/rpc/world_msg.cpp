#include "world/rpc/world_msg.h"

#include "base/time.h"
#include "common/metadata_keys.h"

#include <functional>
#include <utility>

    namespace yuan::game::server
    {
    namespace
    {
        std::uint64_t message_connection_id(const yuan::rpc::Message &message)
        {
            const auto it = message.metadata.find(game_metadata_key::rpc_connection_id);
            if (it == message.metadata.end()) {
                return 0;
            }
            try {
                return static_cast<std::uint64_t>(std::stoull(it->second));
            } catch (...) {
                return 0;
            }
        }

        yuan::rpc::Response deferred_response_for(const yuan::rpc::Message &message)
        {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            response.status = yuan::rpc::RpcStatus::ok;
            response.metadata[game_metadata_key::rpc_defer_response] = "1";
            return response;
        }

        yuan::rpc::Response make_response_for(const yuan::rpc::Message &message)
        {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            return response;
        }
    }

    void world_add_role(WorldMsgContext &context, PlayerUid player_uid, SSPlayerRoleInfo role)
    {
        if (role.zone_service_id == 0) {
            const auto zone = world_player_zone(context, role.role_id);
            if (zone) {
                role.zone_service_id = *zone;
            }
        }
        if (role.world_service_id == 0) {
            role.world_service_id = context.address.service.pack();
        }
        auto &roles = context.roles_by_player_uid[player_uid];
        for (auto &existing : roles) {
            if (existing.role_id == role.role_id) {
                existing = std::move(role);
                return;
            }
        }
        roles.push_back(std::move(role));
    }

    void world_register_gateway(WorldMsgContext &context, SSGatewayInfo gateway)
    {
        for (auto &existing : context.gateways) {
            if (existing.service_id == gateway.service_id) {
                existing = std::move(gateway);
                return;
            }
        }
        context.gateways.push_back(std::move(gateway));
    }

    void world_register_zone(WorldMsgContext &context, SSZoneInfo zone)
    {
        if (zone.service_id == 0) {
            return;
        }
        if (zone.world_routing_strategy != context.world_routing.strategy ||
            zone.world_routing_version != context.world_routing.version ||
            zone.world_count != context.world_routing.world_count) {
            return;
        }
        zone.available = zone.available && !zone.gateways.empty();
        context.zone_last_report_ms[zone.service_id] = yuan::base::time::steady_now_ms();
        context.zones[zone.service_id] = std::move(zone);
    }

    void world_mark_stale_zones_unavailable(WorldMsgContext &context, std::uint64_t now_ms)
    {
        for (auto &[service_id, zone] : context.zones) {
            const auto it = context.zone_last_report_ms.find(service_id);
            if (it == context.zone_last_report_ms.end() || it->second + context.zone_report_ttl_ms <= now_ms) {
                zone.available = false;
            }
        }
    }

    bool world_set_player_zone(WorldMsgContext &context, PlayerId player_id, PackedGameServiceId zone_service_id, PackedGameServiceId source_zone_service_id, std::uint64_t gateway_session_id);

    bool world_set_player_zone(WorldMsgContext &context, PlayerId player_id, PackedGameServiceId zone_service_id)
    {
        return world_set_player_zone(context, player_id, zone_service_id, 0, 0);
    }

    bool world_set_player_zone(WorldMsgContext &context, PlayerId player_id, PackedGameServiceId zone_service_id, PackedGameServiceId source_zone_service_id)
    {
        return world_set_player_zone(context, player_id, zone_service_id, source_zone_service_id, 0);
    }

    bool world_set_player_zone(WorldMsgContext &context, PlayerId player_id, PackedGameServiceId zone_service_id, PackedGameServiceId source_zone_service_id, std::uint64_t gateway_session_id)
    {
        if (player_id == 0) {
            return false;
        }
        PlayerUid player_uid = 0;
        for (const auto &[uid, roles] : context.roles_by_player_uid) {
            for (const auto &role : roles) {
                if (role.role_id == player_id) {
                    player_uid = uid;
                    break;
                }
            }
            if (player_uid != 0) {
                break;
            }
        }
        const auto next = WorldOwnershipRecord{zone_service_id, gateway_session_id};
        bool stale_update = false;
        const auto current_online_role = context.online_by_role.find(player_id);
        if (zone_service_id == 0 && gateway_session_id != 0 && current_online_role != context.online_by_role.end() && current_online_role->second.gateway_session_id != gateway_session_id) {
            stale_update = true;
        }
        if (context.ownership_store) {
            stale_update = stale_update || !context.ownership_store->compare_and_set(player_id, source_zone_service_id, gateway_session_id, next);
        } else {
            const auto current_zone = world_player_zone(context, player_id).value_or(0);
            if (zone_service_id == 0 && source_zone_service_id != 0 && current_zone != 0 && current_zone != source_zone_service_id) {
                stale_update = true;
            }
            const auto current_session_it = context.session_by_player.find(player_id);
            if (zone_service_id == 0 && gateway_session_id != 0 && current_session_it != context.session_by_player.end() && current_session_it->second != gateway_session_id) {
                stale_update = true;
            }
        }
        if (stale_update) {
            return true;
        }
        if (zone_service_id == 0) {
            context.zone_by_player.erase(player_id);
            context.session_by_player.erase(player_id);
            if (current_online_role != context.online_by_role.end()) {
                const auto uid = current_online_role->second.player_uid;
                context.online_by_role.erase(current_online_role);
                const auto uid_it = context.online_by_uid.find(uid);
                if (uid_it != context.online_by_uid.end() && uid_it->second.role_id == player_id) {
                    context.online_by_uid.erase(uid_it);
                }
            }
        } else {
            if (player_uid != 0) {
                const WorldOnlineSession next_session{player_uid, player_id, zone_service_id, gateway_session_id};
                const auto old_uid_session = context.online_by_uid.find(player_uid);
                if (old_uid_session != context.online_by_uid.end() && old_uid_session->second.role_id != player_id) {
                    const auto old_role_id = old_uid_session->second.role_id;
                    context.zone_by_player.erase(old_role_id);
                    context.session_by_player.erase(old_role_id);
                    context.online_by_role.erase(old_role_id);
                    if (context.after_player_zone_set) {
                        context.after_player_zone_set(old_role_id, 0);
                    }
                }
                context.online_by_uid[player_uid] = next_session;
                context.online_by_role[player_id] = next_session;
            }
            context.zone_by_player[player_id] = zone_service_id;
            if (gateway_session_id != 0) {
                context.session_by_player[player_id] = gateway_session_id;
            }
        }
        if (zone_service_id != 0) {
            context.pending_login_by_role.erase(player_id);
        }
        for (auto &[_, roles] : context.roles_by_player_uid) {
            for (auto &role : roles) {
                if (role.role_id == player_id) {
                    role.zone_service_id = zone_service_id;
                }
            }
        }
        if (context.after_player_zone_set) {
            context.after_player_zone_set(player_id, zone_service_id);
        }
        return true;
    }

    void world_prune_expired_login_reservations(WorldMsgContext &context, std::uint64_t now_ms)
    {
        for (auto it = context.pending_login_by_role.begin(); it != context.pending_login_by_role.end();) {
            if (it->second.expires_at_ms <= now_ms) {
                it = context.pending_login_by_role.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::optional<PackedGameServiceId> world_player_zone(const WorldMsgContext &context, PlayerId player_id)
    {
        const auto it = context.zone_by_player.find(player_id);
        if (it == context.zone_by_player.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<PackedGameServiceId> world_select_zone(const WorldMsgContext &context, PlayerUid player_uid, RoleId role_id)
    {
        (void)player_uid;
        const auto current = world_player_zone(context, role_id);
        if (current) {
            const auto it = context.zones.find(*current);
            if (it == context.zones.end() || (it->second.available && (it->second.max_players == 0 || it->second.online_players < it->second.max_players))) {
                return *current;
            }
        }

        const SSZoneInfo *best = nullptr;
        for (const auto &[_, zone] : context.zones) {
            std::uint32_t pending = 0;
            for (const auto &[_, reservation] : context.pending_login_by_role) {
                if (reservation.zone_service_id == zone.service_id) {
                    ++pending;
                }
            }
            const auto effective_online = zone.online_players + pending;
            if (!zone.available || (zone.max_players != 0 && effective_online >= zone.max_players)) {
                continue;
            }
            std::uint32_t best_pending = 0;
            if (best) {
                for (const auto &[_, reservation] : context.pending_login_by_role) {
                    if (reservation.zone_service_id == best->service_id) {
                        ++best_pending;
                    }
                }
            }
            const auto best_effective_online = best ? best->online_players + best_pending : 0;
            if (!best || effective_online < best_effective_online ||
                (effective_online == best_effective_online && zone.service_id < best->service_id)) {
                best = &zone;
            }
        }
        if (!best) {
            return std::nullopt;
        }
        return best->service_id;
    }

    LoginOptionsResponse world_login_options(WorldMsgContext &context, PlayerUid player_uid)
    {
        LoginOptionsResponse response;
        const auto now_ms = yuan::base::time::steady_now_ms();
        world_prune_expired_login_reservations(context, now_ms);
        world_mark_stale_zones_unavailable(context, now_ms);
        response.zones.reserve(context.zones.size());
        for (const auto &[_, zone] : context.zones) {
            response.zones.push_back(zone);
        }
        const auto it = context.roles_by_player_uid.find(player_uid);
        if (it != context.roles_by_player_uid.end()) {
            response.roles = it->second;
            for (auto &role : response.roles) {
                std::optional<PackedGameServiceId> selected_zone;
                const auto pending_it = context.pending_login_by_role.find(role.role_id);
                if (world_player_zone(context, role.role_id).value_or(0) == 0 && pending_it != context.pending_login_by_role.end()) {
                    selected_zone = pending_it->second.zone_service_id;
                    pending_it->second.expires_at_ms = now_ms + context.login_reservation_ttl_ms;
                } else {
                    selected_zone = world_select_zone(context, player_uid, role.role_id);
                }
                if (selected_zone) {
                    role.zone_service_id = *selected_zone;
                    role.login_token_id = encode_login_token_id(*selected_zone, now_ms + context.login_reservation_ttl_ms, context.login_token_secret);
                    if (world_player_zone(context, role.role_id).value_or(0) == 0) {
                        context.pending_login_by_role[role.role_id] = ZoneLoginReservation{*selected_zone, now_ms + context.login_reservation_ttl_ms};
                    }
                    const auto zone_it = context.zones.find(*selected_zone);
                    if (zone_it != context.zones.end() && !zone_it->second.gateways.empty() && response.gateways.empty()) {
                        response.gateways.push_back(zone_it->second.gateways.front());
                    }
                }
            }
        }
        return response;
    }

    namespace
    {
        yuan::rpc::Response handle_world_login_options(WorldMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<LoginOptionsRequest>(message.payload);
            auto response = make_response_for(message);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid login options request";
                return response;
            }
            const auto connection_id = message_connection_id(message);
            if (context.before_login_options_async && context.write_deferred_response && connection_id != 0) {
                const auto player_uid = request->player_uid;
                const auto request_id = message.request_id;
                const auto continuation_id = message.continuation_id();
                context.before_login_options_async(player_uid, [&context, player_uid, request_id, continuation_id, connection_id] {
                    yuan::rpc::Response async_response;
                    async_response.request_id = request_id;
                    async_response.set_continuation_id(continuation_id);
                    async_response.status = yuan::rpc::RpcStatus::ok;
                    (void)encode_binary(world_login_options(context, player_uid), async_response.payload);
                    context.write_deferred_response(connection_id, std::move(async_response));
                });
                return deferred_response_for(message);
            }
            if (context.before_login_options) {
                context.before_login_options(request->player_uid);
            }
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_binary(world_login_options(context, request->player_uid), response.payload);
            return response;
        }

        yuan::rpc::Response handle_world_gateway_register(WorldMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto gateway = decode_binary<SSGatewayInfo>(message.payload);
            auto response = make_response_for(message);
            if (!gateway) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gateway info";
                return response;
            }
            world_register_gateway(context, *gateway);
            response.status = yuan::rpc::RpcStatus::ok;
            return response;
        }

        yuan::rpc::Response handle_world_zone_register(WorldMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto zone = decode_binary<SSZoneInfo>(message.payload);
            auto response = make_response_for(message);
            if (!zone) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid zone info";
                return response;
            }
            world_register_zone(context, *zone);
            response.status = yuan::rpc::RpcStatus::ok;
            return response;
        }

        yuan::rpc::Response handle_world_zone_select(WorldMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSZoneSelectRequest>(message.payload);
            auto response = make_response_for(message);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid zone select request";
                return response;
            }
            const auto zone = world_select_zone(context, request->player_uid, request->role_id).value_or(0);
            (void)encode_binary(SSPlayerZoneUpdate{request->player_uid, request->role_id, zone, 0, 0}, response.payload);
            response.status = zone != 0 ? yuan::rpc::RpcStatus::ok : yuan::rpc::RpcStatus::not_found;
            if (zone == 0) {
                response.error = "no available zone";
            }
            return response;
        }

        yuan::rpc::Response handle_world_player_zone_get(WorldMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto query = decode_binary<SSPlayerZoneQuery>(message.payload);
            auto response = make_response_for(message);
            if (!query) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid player zone query";
                return response;
            }
            const auto zone = world_player_zone(context, query->player_id).value_or(0);
            (void)encode_binary(SSPlayerZoneUpdate{0, query->player_id, zone, 0, 0}, response.payload);
            response.status = yuan::rpc::RpcStatus::ok;
            return response;
        }

        yuan::rpc::Response handle_world_player_zone_set(WorldMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto update = decode_player_zone_update(message.payload);
            auto response = make_response_for(message);
            if (!update) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid player zone update";
                return response;
            }
            world_set_player_zone(context, update->player_id, update->zone_service_id, update->source_zone_service_id, update->gateway_session_id);
            response.status = yuan::rpc::RpcStatus::ok;
            return response;
        }

        yuan::rpc::Response handle_world_gm_forward(WorldMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSGmCommandRequest>(message.payload);
            auto response = make_response_for(message);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gm command request";
                return response;
            }
            if (!context.gm_forward_handler) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "gm forward handler is not configured";
                return response;
            }
            const auto result = context.gm_forward_handler(*request);
            if (!result) {
                response.status = yuan::rpc::RpcStatus::unavailable;
                response.error = "gm target unavailable";
                return response;
            }
            response.status = result->ok ? yuan::rpc::RpcStatus::ok : yuan::rpc::RpcStatus::bad_request;
            (void)encode_binary(*result, response.payload);
            return response;
        }
    }

    bool register_world_msg(yuan::rpc::Server &server, WorldMsgContext &context)
    {
        const bool login_registered = server.register_handler(game_route::world_login_options(), std::bind_front(handle_world_login_options, std::ref(context)));
        const bool gateway_registered = server.register_handler(game_route::world_gateway_register(), std::bind_front(handle_world_gateway_register, std::ref(context)));
        const bool zone_register_registered = server.register_handler(game_route::world_zone_register(), std::bind_front(handle_world_zone_register, std::ref(context)));
        const bool zone_select_registered = server.register_handler(game_route::world_zone_select(), std::bind_front(handle_world_zone_select, std::ref(context)));
        const bool zone_get_registered = server.register_handler(game_route::world_player_zone_get(), std::bind_front(handle_world_player_zone_get, std::ref(context)));
        const bool zone_set_registered = server.register_handler(game_route::world_player_zone_set(), std::bind_front(handle_world_player_zone_set, std::ref(context)));
        const bool gm_registered = server.register_handler(game_route::world_gm_forward(), std::bind_front(handle_world_gm_forward, std::ref(context)));

        return login_registered && gateway_registered && zone_register_registered && zone_select_registered &&
               zone_get_registered && zone_set_registered && gm_registered;
    }
}
