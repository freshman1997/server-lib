#include "game_server.h"

#include <cstdlib>
#include <iostream>
#include <memory>

namespace
{
    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }
}

int main()
{
    using namespace yuan::game::server;

    TunnelCluster tunnels;
    auto tunnel = std::make_shared<TunnelService>(ServiceAddress{{1, 1, GameServiceType::tunnel, 1, 1}, 1, yuan::game_base::ServerRole::gateway, 1, "tunnel"});
    if (!require(tunnels.add(tunnel), "tunnel should be added")) {
        return 1;
    }

    const GameServiceId world_id{1, 1, GameServiceType::world, 1, 1};
    const GameServiceId gateway_id{1, 1, GameServiceType::gateway, 1, 1};
    const GameServiceId web_id{1, 1, GameServiceType::web, 1, 1};
    const GameServiceId zone_id{1, 1, GameServiceType::zone, 1, 1};
    const GameServiceId zone_2_id{1, 1, GameServiceType::zone, 1, 2};

    WorldService world(ServiceAddress{world_id, 400, yuan::game_base::ServerRole::world, 1, "world"});
    GatewayService gateway(ServiceAddress{gateway_id, 500, yuan::game_base::ServerRole::gateway, 1, "gateway"});
    WebService web(ServiceAddress{web_id, 600, yuan::game_base::ServerRole::gm, 1, "web"});
    auto forward = [&](TunnelEnvelope envelope) { return tunnels.forward(std::move(envelope)); };
    ZoneService zone(ServiceAddress{zone_id, 700, yuan::game_base::ServerRole::scene, 1, "zone"}, forward);
    ZoneService zone_2(ServiceAddress{zone_2_id, 701, yuan::game_base::ServerRole::scene, 1, "zone-2"}, forward);

    const PlayerUid player_uid = 90001;
    const RoleId role_id = 10001;
    const RoleId new_role_id = 10002;
    world.add_role(player_uid, PlayerRoleInfo{role_id, "knight", 12, world_id.pack(), zone_id.pack()});
    world.add_role(player_uid, PlayerRoleInfo{new_role_id, "mage", 1, world_id.pack(), 0});
    world.set_player_zone(role_id, zone_id.pack());
    world.register_gateway(GatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"});
    world.register_zone(ZoneInfo{zone_id.pack(), "zone-1", 10, 100, true});
    world.register_zone(ZoneInfo{zone_2_id.pack(), "zone-2", 1, 100, true});

    if (!require(tunnels.register_endpoint(world.address(), world.rpc_server()), "world should register")) {
        return 2;
    }
    if (!require(tunnels.register_endpoint(gateway.address(), gateway.rpc_server()), "gateway should register")) {
        return 3;
    }
    if (!require(tunnels.register_endpoint(zone.address(), zone.rpc_server()), "zone should register")) {
        return 4;
    }
    if (!require(tunnels.register_endpoint(zone_2.address(), zone_2.rpc_server()), "zone-2 should register")) {
        return 13;
    }

    web.set_bootstrap_provider([&](LoginOptionsRequest request) -> std::optional<LoginOptionsResponse> {
        yuan::rpc::Bytes world_payload;
        if (!encode_login_options_request(request, world_payload)) {
            return std::nullopt;
        }
        TunnelEnvelope envelope;
        envelope.source_service_id = web_id.pack();
        envelope.target_service_id = world_id.pack();
        envelope.source = service_id_key(web_id);
        envelope.target = service_id_key(world_id);
        envelope.route = game_route::world_login_options();
        envelope.payload = std::move(world_payload);
        const auto response = tunnel->forward(std::move(envelope));
        if (response.status != yuan::rpc::RpcStatus::ok) {
            return std::nullopt;
        }
        return decode_login_options_response(response.payload);
    });

    gateway.set_login_handler([&](ClientLoginRequest request) -> std::optional<ClientLoginResponse> {
        yuan::rpc::Bytes query_payload;
        if (!encode_zone_select_request(ZoneSelectRequest{request.player_uid, request.role_id}, query_payload)) {
            return std::nullopt;
        }
        TunnelEnvelope world_envelope;
        world_envelope.source_service_id = gateway_id.pack();
        world_envelope.target_service_id = world_id.pack();
        world_envelope.source = service_id_key(gateway_id);
        world_envelope.target = service_id_key(world_id);
        world_envelope.route = game_route::world_zone_select();
        world_envelope.payload = std::move(query_payload);
        const auto world_response = tunnel->forward(std::move(world_envelope));
        if (world_response.status != yuan::rpc::RpcStatus::ok) {
            return std::nullopt;
        }
        const auto zone_target = decode_player_zone_update(world_response.payload);
        if (!zone_target || zone_target->zone_service_id == 0) {
            return ClientLoginResponse{false, request.role_id, 0, "no zone"};
        }

        yuan::rpc::Bytes login_payload;
        if (!encode_client_login_request(request, login_payload)) {
            return std::nullopt;
        }
        TunnelEnvelope zone_envelope;
        zone_envelope.source_service_id = gateway_id.pack();
        zone_envelope.target_service_id = zone_target->zone_service_id;
        zone_envelope.source = service_id_key(gateway_id);
        zone_envelope.target = std::to_string(zone_target->zone_service_id);
        zone_envelope.route = game_route::zone_player_enter();
        zone_envelope.payload = std::move(login_payload);
        const auto zone_response = tunnel->forward(std::move(zone_envelope));
        if (zone_response.status != yuan::rpc::RpcStatus::ok) {
            return std::nullopt;
        }
        return decode_client_login_response(zone_response.payload);
    });

    zone.set_world_zone_update_handler([&](PlayerZoneUpdate update) {
        yuan::rpc::Bytes payload;
        if (!encode_player_zone_update(update, payload)) {
            return false;
        }
        TunnelEnvelope envelope;
        envelope.source_service_id = zone_id.pack();
        envelope.target_service_id = world_id.pack();
        envelope.source = service_id_key(zone_id);
        envelope.target = service_id_key(world_id);
        envelope.route = game_route::world_player_zone_set();
        envelope.payload = std::move(payload);
        const auto response = tunnel->forward(std::move(envelope));
        return response.status == yuan::rpc::RpcStatus::ok;
    });
    zone_2.set_world_zone_update_handler([&](PlayerZoneUpdate update) {
        yuan::rpc::Bytes payload;
        if (!encode_player_zone_update(update, payload)) {
            return false;
        }
        TunnelEnvelope envelope;
        envelope.source_service_id = zone_2_id.pack();
        envelope.target_service_id = world_id.pack();
        envelope.source = service_id_key(zone_2_id);
        envelope.target = service_id_key(world_id);
        envelope.route = game_route::world_player_zone_set();
        envelope.payload = std::move(payload);
        const auto response = tunnel->forward(std::move(envelope));
        return response.status == yuan::rpc::RpcStatus::ok;
    });

    yuan::rpc::Bytes bootstrap_payload;
    if (!require(encode_login_options_request(LoginOptionsRequest{player_uid}, bootstrap_payload), "bootstrap request should encode")) {
        return 5;
    }
    yuan::rpc::Message bootstrap_message;
    bootstrap_message.route = game_route::web_bootstrap();
    bootstrap_message.payload = std::move(bootstrap_payload);
    const auto bootstrap_response = web.rpc_server().handle(bootstrap_message);
    if (!require(bootstrap_response.status == yuan::rpc::RpcStatus::ok, "web bootstrap should succeed")) {
        return 6;
    }
    const auto options = decode_login_options_response(bootstrap_response.payload);
    if (!require(options && options->gateways.size() == 1 && options->roles.size() == 2, "web should return gateways and roles")) {
        return 7;
    }
    if (!require(options->roles.front().role_id == role_id && options->roles.front().zone_service_id == zone_id.pack(), "role should include target zone")) {
        return 8;
    }

    yuan::rpc::Bytes login_payload;
    if (!require(encode_client_login_request(ClientLoginRequest{player_uid, role_id}, login_payload), "login request should encode")) {
        return 9;
    }
    yuan::rpc::Message login_message;
    login_message.route = game_route::gateway_login();
    login_message.payload = std::move(login_payload);
    const auto login_response = gateway.rpc_server().handle(login_message);
    if (!require(login_response.status == yuan::rpc::RpcStatus::ok, "gateway login should succeed")) {
        return 10;
    }
    const auto login = decode_client_login_response(login_response.payload);
    if (!require(login && login->ok && login->role_id == role_id && login->zone_service_id == zone_id.pack(), "existing role zone should be preserved")) {
        return 11;
    }
    if (!require(world.player_zone(role_id).value_or(0) == zone_id.pack(), "world should record role current zone")) {
        return 12;
    }

    yuan::rpc::Bytes new_login_payload;
    if (!require(encode_client_login_request(ClientLoginRequest{player_uid, new_role_id}, new_login_payload), "new role login request should encode")) {
        return 14;
    }
    yuan::rpc::Message new_login_message;
    new_login_message.route = game_route::gateway_login();
    new_login_message.payload = std::move(new_login_payload);
    const auto new_login_response = gateway.rpc_server().handle(new_login_message);
    if (!require(new_login_response.status == yuan::rpc::RpcStatus::ok, "new role gateway login should succeed")) {
        return 15;
    }
    const auto new_login = decode_client_login_response(new_login_response.payload);
    if (!require(new_login && new_login->ok && new_login->role_id == new_role_id && new_login->zone_service_id == zone_2_id.pack(),
                 "new role should select least-loaded zone")) {
        return 16;
    }
    if (!require(world.player_zone(new_role_id).value_or(0) == zone_2_id.pack(), "world should record new role selected zone")) {
        return 17;
    }

    return EXIT_SUCCESS;
}
