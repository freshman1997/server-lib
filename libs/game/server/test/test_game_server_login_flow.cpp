#include "game_server.h"

#include "base/time.h"

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

    yuan::base::time::set_steady_time_for_test(100000);

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

    WorldMsgContext world{ServiceAddress{world_id, 400, yuan::game_base::ServerRole::world, 1, "world"}};
    world.login_reservation_ttl_ms = 3000;
    world.zone_report_ttl_ms = 3000;
    yuan::rpc::Server world_rpc;
    GatewayMsgContext gateway{ServiceAddress{gateway_id, 500, yuan::game_base::ServerRole::gateway, 1, "gateway"}, "127.0.0.1", 30001};
    yuan::rpc::Server gateway_rpc;
    WebHandlerContext web{ServiceAddress{web_id, 600, yuan::game_base::ServerRole::gm, 1, "web"}};
    yuan::rpc::Server web_rpc;
    const ServiceAddress zone_address{zone_id, 700, yuan::game_base::ServerRole::scene, 1, "zone"};
    const ServiceAddress zone_2_address{zone_2_id, 701, yuan::game_base::ServerRole::scene, 1, "zone-2"};
    yuan::rpc::Server zone_rpc;
    yuan::rpc::Server zone_2_rpc;

    const PlayerUid player_uid = 90001;
    const RoleId role_id = 10001;
    const RoleId new_role_id = 10002;
    world_add_role(world, player_uid, PlayerRoleInfo{role_id, "knight", 12, world_id.pack(), zone_id.pack()});
    world_add_role(world, player_uid, PlayerRoleInfo{new_role_id, "mage", 1, world_id.pack(), 0});
    world_set_player_zone(world, role_id, zone_id.pack());
    world_register_gateway(world, GatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"});
    world_register_zone(world, ZoneInfo{zone_id.pack(), "zone-1", 10, 100, true, {GatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"}}});
    world_register_zone(world, ZoneInfo{zone_2_id.pack(), "zone-2", 1, 100, true, {GatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"}}});

    if (!require(register_world_msg(world_rpc, world), "world handlers should register")) {
        return 20;
    }

    if (!require(tunnels.register_endpoint(world.address, world_rpc), "world should register")) {
        return 2;
    }
    if (!require(tunnels.register_endpoint(gateway.address, gateway_rpc), "gateway should register")) {
        return 3;
    }
    auto update_world_from_zone = [&](GameServiceId source_id, PlayerZoneUpdate update) {
        yuan::rpc::Bytes payload;
        if (!encode_player_zone_update(update, payload)) {
            return false;
        }
        TunnelEnvelope envelope;
        envelope.source_service_id = source_id.pack();
        envelope.target_service_id = world_id.pack();
        envelope.source = service_id_key(source_id);
        envelope.target = service_id_key(world_id);
        envelope.route = game_route::world_player_zone_set();
        envelope.payload = std::move(payload);
        const auto response = tunnel->forward(std::move(envelope));
        return response.status == yuan::rpc::RpcStatus::ok;
    };

    if (!require(register_zone_msg_player(zone_rpc, zone_address, ZoneMsgPlayerHandlers{
                      [&](PlayerZoneUpdate update) { return update_world_from_zone(zone_id, update); },
                      {},
                      {}}) &&
                  register_zone_msg_player(zone_2_rpc, zone_2_address, ZoneMsgPlayerHandlers{
                      [&](PlayerZoneUpdate update) { return update_world_from_zone(zone_2_id, update); },
                      {},
                      {}}) &&
                  register_zone_msg_echo(zone_rpc, zone_address) && register_zone_msg_echo(zone_2_rpc, zone_2_address),
                  "zone player handlers should register")) {
        return 18;
    }

    if (!require(tunnels.register_endpoint(zone_address, zone_rpc), "zone should register")) {
        return 4;
    }
    if (!require(tunnels.register_endpoint(zone_2_address, zone_2_rpc), "zone-2 should register")) {
        return 13;
    }

    web.bootstrap_provider = [&](LoginOptionsRequest request) -> std::optional<LoginOptionsResponse> {
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
    };

    gateway.login_handler = [&](ClientLoginRequest request) -> std::optional<ClientLoginResponse> {
        if (request.zone_service_id == 0) {
            return ClientLoginResponse{false, request.role_id, 0, 0, "no zone"};
        }

        yuan::rpc::Bytes login_payload;
        if (!encode_client_login_request(request, login_payload)) {
            return std::nullopt;
        }
        TunnelEnvelope zone_envelope;
        zone_envelope.source_service_id = gateway_id.pack();
        zone_envelope.target_service_id = request.zone_service_id;
        zone_envelope.source = service_id_key(gateway_id);
        zone_envelope.target = std::to_string(request.zone_service_id);
        zone_envelope.route = game_route::zone_player_enter();
        zone_envelope.payload = std::move(login_payload);
        const auto zone_response = tunnel->forward(std::move(zone_envelope));
        if (zone_response.status != yuan::rpc::RpcStatus::ok) {
            return std::nullopt;
        }
        return decode_client_login_response(zone_response.payload);
    };
    gateway.game_forward_handler = [&](ClientGameRequest request, PackedGameServiceId zone_service_id) -> std::optional<ClientGameResponse> {
        TunnelEnvelope zone_envelope;
        zone_envelope.source_service_id = gateway_id.pack();
        zone_envelope.target_service_id = zone_service_id;
        zone_envelope.source = service_id_key(gateway_id);
        zone_envelope.target = std::to_string(zone_service_id);
        zone_envelope.route = game_route::zone_echo();
        zone_envelope.payload = request.payload;
        zone_envelope.metadata["gateway.session_id"] = std::to_string(request.gateway_session_id);
        const auto zone_response = tunnel->forward(std::move(zone_envelope));
        if (zone_response.status != yuan::rpc::RpcStatus::ok) {
            return std::nullopt;
        }
        return ClientGameResponse{true, request.role_id, request.gateway_session_id, zone_response.payload, "zone game ok"};
    };
    if (!require(register_gateway_msg_client(gateway_rpc, gateway), "gateway handlers should register")) {
        return 19;
    }
    if (!require(register_web_handlers(web_rpc, web), "web handlers should register")) {
        return 21;
    }

    yuan::rpc::Bytes bootstrap_payload;
    if (!require(encode_login_options_request(LoginOptionsRequest{player_uid}, bootstrap_payload), "bootstrap request should encode")) {
        return 5;
    }
    yuan::rpc::Message bootstrap_message;
    bootstrap_message.route = game_route::web_bootstrap();
    bootstrap_message.payload = std::move(bootstrap_payload);
    const auto bootstrap_response = web_rpc.handle(bootstrap_message);
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
    if (!require(encode_client_login_request(ClientLoginRequest{player_uid, role_id, zone_id.pack(), 0}, login_payload), "login request should encode")) {
        return 9;
    }
    yuan::rpc::Message login_message;
    login_message.route = game_route::gateway_login();
    login_message.payload = std::move(login_payload);
    const auto login_response = gateway_rpc.handle(login_message);
    if (!require(login_response.status == yuan::rpc::RpcStatus::ok, "gateway login should succeed")) {
        return 10;
    }
    const auto login = decode_client_login_response(login_response.payload);
    if (!require(login && login->ok && login->role_id == role_id && login->zone_service_id == zone_id.pack(), "existing role zone should be preserved")) {
        return 11;
    }
    if (!require(login->gateway_session_id != 0, "gateway login should allocate session id")) {
        return 22;
    }
    if (!require(world_player_zone(world, role_id).value_or(0) == zone_id.pack(), "world should record role current zone")) {
        return 12;
    }

    yuan::rpc::Bytes invalid_game_payload;
    if (!require(encode_client_game_request(ClientGameRequest{player_uid,
                                                              role_id,
                                                              login->gateway_session_id + 1000,
                                                              yuan::rpc::Codec<std::string>::encode("move:bad")},
                                            invalid_game_payload),
                 "invalid session game request should encode")) {
        return 23;
    }
    yuan::rpc::Message invalid_game_message;
    invalid_game_message.route = game_route::gateway_game_forward();
    invalid_game_message.payload = std::move(invalid_game_payload);
    const auto invalid_game_response = gateway_rpc.handle(invalid_game_message);
    if (!require(invalid_game_response.status == yuan::rpc::RpcStatus::not_found, "gateway should reject invalid session id")) {
        return 24;
    }

    yuan::rpc::Bytes header_mismatch_payload;
    if (!require(encode_client_game_request(ClientGameRequest{player_uid,
                                                              role_id,
                                                              login->gateway_session_id,
                                                              yuan::rpc::Codec<std::string>::encode("move:framed")},
                                            header_mismatch_payload),
                 "framed session game request should encode")) {
        return 40;
    }
    yuan::rpc::Message header_mismatch_message;
    header_mismatch_message.route = game_route::gateway_game_forward();
    header_mismatch_message.payload = encode_framed_client_payload(
        ClientFrameHeader{player_uid, role_id, zone_id.pack(), login->gateway_session_id + 1, 1, header_mismatch_message.route.service, header_mismatch_message.route.method},
        std::move(header_mismatch_payload));
    const auto header_mismatch_response = gateway_rpc.handle(header_mismatch_message);
    if (!require(header_mismatch_response.status == yuan::rpc::RpcStatus::bad_request, "gateway should reject mismatched CS frame header")) {
        return 41;
    }
    if (!require(header_mismatch_response.metadata.find(rpc_network::metadata_key::close_connection) != header_mismatch_response.metadata.end(),
                 "gateway protocol violation should request connection close")) {
        return 47;
    }

    yuan::rpc::Bytes framed_game_payload;
    if (!require(encode_client_game_request(ClientGameRequest{player_uid,
                                                              role_id,
                                                              login->gateway_session_id,
                                                              yuan::rpc::Codec<std::string>::encode("move:framed")},
                                            framed_game_payload),
                 "valid framed game request should encode")) {
        return 42;
    }
    yuan::rpc::Message framed_game_message;
    framed_game_message.route = game_route::gateway_game_forward();
    framed_game_message.payload = encode_framed_client_payload(
        ClientFrameHeader{player_uid, role_id, zone_id.pack(), login->gateway_session_id, 1, framed_game_message.route.service, framed_game_message.route.method},
        std::move(framed_game_payload));
    const auto framed_game_response = gateway_rpc.handle(framed_game_message);
    if (!require(framed_game_response.status == yuan::rpc::RpcStatus::ok, "gateway should accept valid CS frame header")) {
        return 43;
    }

    yuan::rpc::Message replay_game_message;
    replay_game_message.route = game_route::gateway_game_forward();
    replay_game_message.payload = framed_game_message.payload;
    const auto replay_game_response = gateway_rpc.handle(replay_game_message);
    if (!require(replay_game_response.status == yuan::rpc::RpcStatus::bad_request, "gateway should reject replayed CS frame sequence")) {
        return 44;
    }

    bool pushed_to_gateway = false;
    gateway.push_handler = [&](ClientPushMessage push) {
        pushed_to_gateway = push.role_id == role_id && push.gateway_session_id == login->gateway_session_id && yuan::rpc::Codec<std::string>::decode(push.payload) == "server-event";
        return pushed_to_gateway;
    };
    yuan::rpc::Bytes push_payload;
    if (!require(encode_client_push_message(ClientPushMessage{role_id, login->gateway_session_id, yuan::rpc::Codec<std::string>::encode("server-event"), "push"}, push_payload),
                 "gateway push payload should encode")) {
        return 45;
    }
    yuan::rpc::Message push_message;
    push_message.route = game_route::gateway_push();
    push_message.payload = std::move(push_payload);
    const auto push_response = gateway_rpc.handle(push_message);
    if (!require(push_response.status == yuan::rpc::RpcStatus::ok && pushed_to_gateway, "gateway should accept push for active session")) {
        return 46;
    }

    yuan::rpc::Bytes new_login_payload;
    if (!require(encode_client_login_request(ClientLoginRequest{player_uid, new_role_id, zone_2_id.pack(), 0}, new_login_payload), "new role login request should encode")) {
        return 14;
    }
    yuan::rpc::Message new_login_message;
    new_login_message.route = game_route::gateway_login();
    new_login_message.payload = std::move(new_login_payload);
    const auto new_login_response = gateway_rpc.handle(new_login_message);
    if (!require(new_login_response.status == yuan::rpc::RpcStatus::ok, "new role gateway login should succeed")) {
        return 15;
    }
    const auto new_login = decode_client_login_response(new_login_response.payload);
    if (!require(new_login && new_login->ok && new_login->role_id == new_role_id && new_login->zone_service_id == zone_2_id.pack(),
                 "new role should select least-loaded zone")) {
        return 16;
    }
    if (!require(new_login->gateway_session_id != 0 && new_login->gateway_session_id != login->gateway_session_id,
                 "gateway should allocate distinct session ids")) {
        return 25;
    }
    if (!require(world_player_zone(world, new_role_id).value_or(0) == zone_2_id.pack(), "world should record new role selected zone")) {
        return 17;
    }

    const PlayerUid burst_player_uid_1 = 90002;
    const PlayerUid burst_player_uid_2 = 90003;
    const RoleId burst_role_1 = 10003;
    const RoleId burst_role_2 = 10004;
    world_add_role(world, burst_player_uid_1, PlayerRoleInfo{burst_role_1, "rogue", 1, world_id.pack(), 0});
    world_add_role(world, burst_player_uid_2, PlayerRoleInfo{burst_role_2, "priest", 1, world_id.pack(), 0});
    world_register_zone(world, ZoneInfo{zone_id.pack(), "zone-1", 0, 1, true, {GatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"}}});
    world_register_zone(world, ZoneInfo{zone_2_id.pack(), "zone-2", 0, 1, true, {GatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"}}});
    const auto first_burst_options = world_login_options(world, burst_player_uid_1);
    const auto second_burst_options = world_login_options(world, burst_player_uid_2);
    const auto duplicate_first_burst_options = world_login_options(world, burst_player_uid_1);
    PackedGameServiceId first_burst_zone = 0;
    PackedGameServiceId second_burst_zone = 0;
    for (const auto &role : first_burst_options.roles) {
        if (role.role_id == burst_role_1) {
            first_burst_zone = role.zone_service_id;
        }
    }
    for (const auto &role : second_burst_options.roles) {
        if (role.role_id == burst_role_2) {
            second_burst_zone = role.zone_service_id;
        }
    }
    if (!require(first_burst_zone != 0 && second_burst_zone != 0 && first_burst_zone != second_burst_zone,
                 "pending reservations should spread burst login options across capacity")) {
        return 28;
    }
    PackedGameServiceId duplicate_first_burst_zone = 0;
    for (const auto &role : duplicate_first_burst_options.roles) {
        if (role.role_id == burst_role_1) {
            duplicate_first_burst_zone = role.zone_service_id;
        }
    }
    if (!require(duplicate_first_burst_zone == first_burst_zone, "duplicate login options should refresh the same role reservation")) {
        return 32;
    }

    yuan::base::time::advance_steady_time_for_test(4000);
    world_prune_expired_login_reservations(world, yuan::base::time::steady_now_ms());
    if (!require(world.pending_login_by_role.empty(), "expired pending login reservations should be pruned")) {
        return 33;
    }

    if (!require(world_set_player_zone(world, burst_role_1, zone_2_id.pack(), zone_2_id.pack(), 200), "new zone should own role")) {
        return 29;
    }
    if (!require(world_set_player_zone(world, burst_role_1, 0, zone_id.pack(), 100), "old zone logout should be ignored")) {
        return 30;
    }
    if (!require(world_player_zone(world, burst_role_1).value_or(0) == zone_2_id.pack(), "old zone logout must not clear new zone ownership")) {
        return 31;
    }
    if (!require(world_set_player_zone(world, burst_role_1, zone_2_id.pack(), zone_2_id.pack(), 201), "same zone newer login should update session fence")) {
        return 34;
    }
    if (!require(world_set_player_zone(world, burst_role_1, 0, zone_2_id.pack(), 200), "same zone stale logout should be ignored")) {
        return 35;
    }
    if (!require(world_player_zone(world, burst_role_1).value_or(0) == zone_2_id.pack(), "same zone stale logout must not clear newer session")) {
        return 36;
    }

    world_register_zone(world, ZoneInfo{zone_id.pack(), "zone-1", 0, 100, true, {GatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"}}});
    world_register_zone(world, ZoneInfo{zone_2_id.pack(), "zone-2", 3, 100, true, {GatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"}}});
    const auto reselected_zone = world_select_zone(world, player_uid, new_role_id);
    if (!require(reselected_zone && *reselected_zone == zone_2_id.pack(), "logged-in role should keep current zone even if another zone is less loaded")) {
        return 26;
    }

    world_set_player_zone(world, new_role_id, 0);
    const auto least_loaded_zone = world_select_zone(world, player_uid, new_role_id);
    if (!require(least_loaded_zone && *least_loaded_zone == zone_id.pack(), "new login should use refreshed least-loaded zone")) {
        return 27;
    }

    yuan::base::time::advance_steady_time_for_test(4000);
    world_mark_stale_zones_unavailable(world, yuan::base::time::steady_now_ms());
    if (!require(!world.zones[zone_id.pack()].available && !world.zones[zone_2_id.pack()].available,
                 "stale zones should become unavailable without periodic reports")) {
        return 37;
    }
    if (!require(!world_select_zone(world, player_uid, new_role_id), "world should not select stale zones")) {
        return 38;
    }
    world_register_zone(world, ZoneInfo{zone_id.pack(), "zone-1", 0, 100, true, {GatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"}}});
    const auto fresh_zone = world_select_zone(world, player_uid, new_role_id);
    if (!require(fresh_zone && *fresh_zone == zone_id.pack(), "fresh zone report should make zone selectable again")) {
        return 39;
    }

    yuan::base::time::reset_test_time();
    return EXIT_SUCCESS;
}
