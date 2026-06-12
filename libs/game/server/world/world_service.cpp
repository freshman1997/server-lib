#include "world/world_service.h"

#include <utility>

namespace yuan::game::server
{
    WorldService::WorldService(ServiceAddress address)
        : ServiceNode(std::move(address))
    {
        (void)rpc_server().register_handler(game_route::world_login_options(), [this](const yuan::rpc::Message &message) {
            const auto request = decode_login_options_request(message.payload);
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid login options request";
                return response;
            }
            if (before_login_options_) {
                before_login_options_(request->player_uid);
            }
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_login_options_response(login_options(request->player_uid), response.payload);
            return response;
        });

        (void)rpc_server().register_handler(game_route::world_gateway_register(), [this](const yuan::rpc::Message &message) {
            const auto gateway = decode_gateway_info(message.payload);
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            if (!gateway) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gateway info";
                return response;
            }
            register_gateway(*gateway);
            response.status = yuan::rpc::RpcStatus::ok;
            return response;
        });

        (void)rpc_server().register_handler(game_route::world_zone_register(), [this](const yuan::rpc::Message &message) {
            const auto zone = decode_zone_info(message.payload);
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            if (!zone) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid zone info";
                return response;
            }
            register_zone(*zone);
            response.status = yuan::rpc::RpcStatus::ok;
            return response;
        });

        (void)rpc_server().register_handler(game_route::world_zone_select(), [this](const yuan::rpc::Message &message) {
            const auto request = decode_zone_select_request(message.payload);
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid zone select request";
                return response;
            }
            const auto zone = select_zone(request->player_uid, request->role_id).value_or(0);
            (void)encode_player_zone_update(PlayerZoneUpdate{request->role_id, zone}, response.payload);
            response.status = zone != 0 ? yuan::rpc::RpcStatus::ok : yuan::rpc::RpcStatus::not_found;
            if (zone == 0) {
                response.error = "no available zone";
            }
            return response;
        });

        (void)rpc_server().register_handler(game_route::world_player_zone_get(), [this](const yuan::rpc::Message &message) {
            const auto query = decode_player_zone_query(message.payload);
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            if (!query) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid player zone query";
                return response;
            }
            const auto zone = player_zone(query->player_id).value_or(0);
            (void)encode_player_zone_update(PlayerZoneUpdate{query->player_id, zone}, response.payload);
            response.status = yuan::rpc::RpcStatus::ok;
            return response;
        });

        (void)rpc_server().register_handler(game_route::world_player_zone_set(), [this](const yuan::rpc::Message &message) {
            const auto update = decode_player_zone_update(message.payload);
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            if (!update) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid player zone update";
                return response;
            }
            set_player_zone(update->player_id, update->zone_service_id);
            response.status = yuan::rpc::RpcStatus::ok;
            return response;
        });

        (void)rpc_server().register_handler(game_route::world_gm_forward(), [this](const yuan::rpc::Message &message) {
            const auto request = decode_gm_command_request(message.payload);
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gm command request";
                return response;
            }
            if (!gm_forward_handler_) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "gm forward handler is not configured";
                return response;
            }
            const auto result = gm_forward_handler_(*request);
            if (!result) {
                response.status = yuan::rpc::RpcStatus::unavailable;
                response.error = "gm target unavailable";
                return response;
            }
            response.status = result->ok ? yuan::rpc::RpcStatus::ok : yuan::rpc::RpcStatus::bad_request;
            (void)encode_gm_command_response(*result, response.payload);
            return response;
        });
    }

    void WorldService::add_role(PlayerUid player_uid, PlayerRoleInfo role)
    {
        if (role.zone_service_id == 0) {
            const auto zone = player_zone(role.role_id);
            if (zone) {
                role.zone_service_id = *zone;
            }
        }
        if (role.world_service_id == 0) {
            role.world_service_id = address().service.pack();
        }
        auto &roles = roles_by_player_uid_[player_uid];
        for (auto &existing : roles) {
            if (existing.role_id == role.role_id) {
                existing = std::move(role);
                return;
            }
        }
        roles.push_back(std::move(role));
    }

    void WorldService::register_gateway(GatewayInfo gateway)
    {
        for (auto &existing : gateways_) {
            if (existing.service_id == gateway.service_id) {
                existing = std::move(gateway);
                return;
            }
        }
        gateways_.push_back(std::move(gateway));
    }

    void WorldService::register_zone(ZoneInfo zone)
    {
        if (zone.service_id == 0) {
            return;
        }
        zones_[zone.service_id] = std::move(zone);
    }

    bool WorldService::set_player_zone(PlayerId player_id, PackedGameServiceId zone_service_id)
    {
        if (player_id == 0) {
            return false;
        }
        zone_by_player_[player_id] = zone_service_id;
        auto role_zone = player_zone(player_id).value_or(zone_service_id);
        (void)role_zone;
        for (auto &[_, roles] : roles_by_player_uid_) {
            for (auto &role : roles) {
                if (role.role_id == player_id) {
                    role.zone_service_id = zone_service_id;
                }
            }
        }
        if (after_player_zone_set_) {
            after_player_zone_set_(player_id, zone_service_id);
        }
        return true;
    }

    void WorldService::set_before_login_options(std::function<void(PlayerUid)> handler)
    {
        before_login_options_ = std::move(handler);
    }

    void WorldService::set_after_player_zone_set(std::function<void(PlayerId, PackedGameServiceId)> handler)
    {
        after_player_zone_set_ = std::move(handler);
    }

    void WorldService::set_gm_forward_handler(std::function<std::optional<GmCommandResponse>(GmCommandRequest)> handler)
    {
        gm_forward_handler_ = std::move(handler);
    }

    std::optional<PackedGameServiceId> WorldService::player_zone(PlayerId player_id) const
    {
        const auto it = zone_by_player_.find(player_id);
        if (it == zone_by_player_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<PackedGameServiceId> WorldService::select_zone(PlayerUid player_uid, RoleId role_id) const
    {
        (void)player_uid;
        const auto current = player_zone(role_id);
        if (current) {
            const auto it = zones_.find(*current);
            if (it == zones_.end() || (it->second.available && (it->second.max_players == 0 || it->second.online_players < it->second.max_players))) {
                return *current;
            }
        }

        const ZoneInfo *best = nullptr;
        for (const auto &[_, zone] : zones_) {
            if (!zone.available || (zone.max_players != 0 && zone.online_players >= zone.max_players)) {
                continue;
            }
            if (!best || zone.online_players < best->online_players ||
                (zone.online_players == best->online_players && zone.service_id < best->service_id)) {
                best = &zone;
            }
        }
        if (!best) {
            return std::nullopt;
        }
        return best->service_id;
    }

    LoginOptionsResponse WorldService::login_options(PlayerUid player_uid) const
    {
        LoginOptionsResponse response;
        response.gateways = gateways_;
        const auto it = roles_by_player_uid_.find(player_uid);
        if (it != roles_by_player_uid_.end()) {
            response.roles = it->second;
            for (auto &role : response.roles) {
                const auto zone = player_zone(role.role_id);
                if (zone) {
                    role.zone_service_id = *zone;
                }
            }
        }
        return response;
    }
}
