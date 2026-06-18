#include "base/time.h"
#include "common/codec/game_binary_codec.h"
#include "common/game_rpc_protocol.h"
#include "common/login_token.h"
#include "common/metadata_keys.h"
#include "common/proto/client_proto.h"
#include "common/proto/login_proto.h"
#include "common/rpc_network.h"
#include "gateway/rpc/gateway_msg_client.h"
#include "tunnel/rpc/tunnel_service.h"
#include "world/rpc/world_msg.h"
#include "zone/rpc/zone_msg_echo.h"
#include "zone/rpc/zone_msg_player.h"

#include <cstdlib>
#include <iostream>
#include <unordered_map>
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
    const GameServiceId zone_id{1, 1, GameServiceType::zone, 1, 1};
    const GameServiceId zone_2_id{1, 1, GameServiceType::zone, 1, 2};

    WorldMsgContext world{ServiceAddress{world_id, 400, yuan::game_base::ServerRole::world, 1, "world"}};
    world.login_reservation_ttl_ms = 3000;
    world.zone_report_ttl_ms = 3000;
    yuan::rpc::Server world_rpc;
    GatewayMsgContext gateway{ServiceAddress{gateway_id, 500, yuan::game_base::ServerRole::gateway, 1, "gateway"}, "127.0.0.1", 30001};
    yuan::rpc::Server gateway_rpc;
    const ServiceAddress zone_address{zone_id, 700, yuan::game_base::ServerRole::scene, 1, "zone"};
    const ServiceAddress zone_2_address{zone_2_id, 701, yuan::game_base::ServerRole::scene, 1, "zone-2"};
    yuan::rpc::Server zone_rpc;
    yuan::rpc::Server zone_2_rpc;
    std::unordered_map<std::uint64_t, RoleId> zone_session_roles;
    std::unordered_map<std::uint64_t, RoleId> zone_2_session_roles;

    const PlayerUid player_uid = 90001;
    const RoleId role_id = 10001;
    const RoleId new_role_id = 10002;
    world_add_role(world, player_uid, SSPlayerRoleInfo{role_id, "knight", 12, world_id.pack(), zone_id.pack()});
    world_add_role(world, player_uid, SSPlayerRoleInfo{new_role_id, "mage", 1, world_id.pack(), 0});
    world_set_player_zone(world, role_id, zone_id.pack());
    world_register_gateway(world, SSGatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"});
    world_register_zone(world, SSZoneInfo{GameServiceId{1, 1, GameServiceType::zone, 1, 99}.pack(), "bad-routing-zone", 1, 100, true, {SSGatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"}}, "modulo", 2, 1});
    if (!require(world.zones.empty(), "world should reject zone with mismatched routing config")) {
        return 58;
    }
    world_register_zone(world, SSZoneInfo{zone_id.pack(), "zone-1", 10, 100, true, {SSGatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"}}});
    world_register_zone(world, SSZoneInfo{zone_2_id.pack(), "zone-2", 1, 100, true, {SSGatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"}}});

    if (!require(register_world_msg(world_rpc, world), "world handlers should register")) {
        return 20;
    }

    if (!require(tunnels.register_endpoint(world.address, world_rpc), "world should register")) {
        return 2;
    }
    if (!require(tunnels.register_endpoint(gateway.address, gateway_rpc), "gateway should register")) {
        return 3;
    }
    auto update_world_from_zone = [&](GameServiceId source_id, SSPlayerZoneUpdate update) {
        yuan::rpc::Bytes payload;
        if (!encode_binary(update, payload)) {
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
                      [&](SSPlayerZoneUpdate update) { return update_world_from_zone(zone_id, update); },
                      [&](SSGatewayLoginRequest request) {
                          zone_session_roles[request.gateway_session_id] = request.role_id;
                          return true;
                      },
                      [&](SSGatewayLoginRequest request) {
                          zone_session_roles.erase(request.gateway_session_id);
                          return true;
                      },
                      [&](std::uint64_t gateway_session_id) {
                          const auto it = zone_session_roles.find(gateway_session_id);
                          return it == zone_session_roles.end() ? RoleId{} : it->second;
                      }}) &&
                  register_zone_msg_player(zone_2_rpc, zone_2_address, ZoneMsgPlayerHandlers{
                      [&](SSPlayerZoneUpdate update) { return update_world_from_zone(zone_2_id, update); },
                      [&](SSGatewayLoginRequest request) {
                          zone_2_session_roles[request.gateway_session_id] = request.role_id;
                          return true;
                      },
                      [&](SSGatewayLoginRequest request) {
                          zone_2_session_roles.erase(request.gateway_session_id);
                          return true;
                      },
                      [&](std::uint64_t gateway_session_id) {
                          const auto it = zone_2_session_roles.find(gateway_session_id);
                          return it == zone_2_session_roles.end() ? RoleId{} : it->second;
                      }}) &&
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

    std::uint32_t selected_login_count = 0;
    auto select_login_zone = [&]() {
        ++selected_login_count;
        return selected_login_count == 2 ? zone_2_id.pack() : zone_id.pack();
    };
    auto forward_to_zone = [&](PackedGameServiceId zone_service_id,
                               yuan::rpc::Route route,
                               GatewayForwardContext context,
                               yuan::rpc::Bytes payload) -> std::optional<yuan::rpc::Response> {
        TunnelEnvelope zone_envelope;
        zone_envelope.source_service_id = gateway_id.pack();
        zone_envelope.target_service_id = zone_service_id;
        zone_envelope.source = service_id_key(gateway_id);
        zone_envelope.target = std::to_string(zone_service_id);
        zone_envelope.route = std::move(route);
        zone_envelope.payload = std::move(payload);
        zone_envelope.metadata[game_metadata_key::gateway_session_id] = std::to_string(context.gateway_session_id);
        const auto zone_response = tunnel->forward(std::move(zone_envelope));
        if (zone_response.status != yuan::rpc::RpcStatus::ok) {
            return std::nullopt;
        }
        return zone_response;
    };
    bool pushed_to_gateway = false;
    bool closed_gateway_session = false;
    std::uint64_t expected_push_session = 0;
    auto push_to_client = [&](std::uint64_t push_session_id, yuan::rpc::Bytes payload) {
        pushed_to_gateway = push_session_id == expected_push_session && yuan::rpc::Codec<std::string>::decode(payload) == "server-event";
        return pushed_to_gateway;
    };
    auto close_session = [&](std::uint64_t gateway_session_id) {
        closed_gateway_session = gateway_session_id == expected_push_session;
        gateway.sessions.logout_session(gateway_session_id);
        return closed_gateway_session;
    };
    if (!require(register_gateway_msg_client(gateway_rpc, gateway, select_login_zone, forward_to_zone, push_to_client, close_session), "gateway handlers should register")) {
        return 19;
    }
    auto valid_login_token = [](PackedGameServiceId zone_service_id) {
        return encode_login_token_id(zone_service_id, yuan::base::time::steady_now_ms() + 10000, kDefaultLoginTokenSecret);
    };

    const auto options = world_login_options(world, player_uid);
    if (!require(options.gateways.size() == 1 && options.roles.size() == 2, "world should return gateways and roles")) {
        return 7;
    }
    if (!require(options.roles.front().role_id == role_id && decode_login_token_id(options.roles.front().login_token_id, yuan::base::time::steady_now_ms(), kDefaultLoginTokenSecret) == zone_id.pack(), "role should include login token")) {
        return 8;
    }

    yuan::rpc::Bytes login_payload;
    if (!require(encode_binary(CSLoginRequest{player_uid, role_id, valid_login_token(zone_id.pack()), 0}, login_payload), "login request should encode")) {
        return 9;
    }
    yuan::rpc::Message login_message;
    login_message.route = game_route::gateway_login();
    login_message.payload = std::move(login_payload);
    login_message.metadata[rpc_network::metadata_key::connection_id] = "1001";
    const auto login_response = gateway_rpc.handle(login_message);
    if (!require(login_response.status == yuan::rpc::RpcStatus::ok, "gateway login should succeed")) {
        return 10;
    }
    const auto login = decode_binary<CSLoginResponse>(login_response.payload);
    if (!require(login && login->ok && login->role_id == role_id && login->zone_service_id == 0 && login->gateway_session_id == 0,
                 "client login response should hide internal zone and session")) {
        return 11;
    }
    const auto internal_login = gateway.sessions.session_for_connection(1001);
    if (!require(internal_login && internal_login->gateway_session_id != 0 && internal_login->zone_service_id == zone_id.pack(),
                 "gateway should allocate internal session context")) {
        return 22;
    }
    if (!require(world_player_zone(world, role_id).value_or(0) == zone_id.pack(), "world should record role current zone")) {
        return 12;
    }

    yuan::rpc::Bytes tampered_login_payload;
    auto tampered_token = options.roles.front().login_token_id;
    tampered_token.back() = tampered_token.back() == '0' ? '1' : '0';
    if (!require(encode_binary(CSLoginRequest{player_uid, role_id, tampered_token, 0}, tampered_login_payload), "tampered login request should encode")) {
        return 56;
    }
    yuan::rpc::Message tampered_login_message;
    tampered_login_message.route = game_route::gateway_login();
    tampered_login_message.payload = std::move(tampered_login_payload);
    tampered_login_message.metadata[rpc_network::metadata_key::connection_id] = "2001";
    const auto tampered_login_response = gateway_rpc.handle(tampered_login_message);
    if (!require(tampered_login_response.status == yuan::rpc::RpcStatus::bad_request,
                 "gateway should reject tampered login token")) {
        return 57;
    }

    yuan::rpc::Bytes wrong_secret_login_payload;
    if (!require(encode_binary(CSLoginRequest{player_uid, role_id, encode_login_token_id(zone_id.pack(), yuan::base::time::steady_now_ms() + 10000, kDefaultLoginTokenSecret + 1), 0}, wrong_secret_login_payload),
                 "wrong-secret login request should encode")) {
        return 58;
    }
    yuan::rpc::Message wrong_secret_login_message;
    wrong_secret_login_message.route = game_route::gateway_login();
    wrong_secret_login_message.payload = std::move(wrong_secret_login_payload);
    wrong_secret_login_message.metadata[rpc_network::metadata_key::connection_id] = "2002";
    const auto wrong_secret_login_response = gateway_rpc.handle(wrong_secret_login_message);
    if (!require(wrong_secret_login_response.status == yuan::rpc::RpcStatus::bad_request,
                 "gateway should reject login token signed with another secret")) {
        return 59;
    }

    yuan::rpc::Bytes expired_login_payload;
    if (!require(encode_binary(CSLoginRequest{player_uid, role_id, encode_login_token_id(zone_id.pack(), yuan::base::time::steady_now_ms(), kDefaultLoginTokenSecret), 0}, expired_login_payload),
                 "expired login request should encode")) {
        return 65;
    }
    yuan::rpc::Message expired_login_message;
    expired_login_message.route = game_route::gateway_login();
    expired_login_message.payload = std::move(expired_login_payload);
    expired_login_message.metadata[rpc_network::metadata_key::connection_id] = "2003";
    const auto expired_login_response = gateway_rpc.handle(expired_login_message);
    if (!require(expired_login_response.status == yuan::rpc::RpcStatus::bad_request,
                 "gateway should reject expired login token")) {
        return 66;
    }

    yuan::rpc::Bytes invalid_game_payload;
    if (!require(encode_binary(CSGameRequest{player_uid,
                                                              role_id,
                                                              0,
                                                              yuan::rpc::Codec<std::string>::encode("move:bad")},
                                            invalid_game_payload),
                 "invalid session game request should encode")) {
        return 23;
    }
    yuan::rpc::Message invalid_game_message;
    invalid_game_message.route = game_route::gateway_game_forward();
    invalid_game_message.payload = std::move(invalid_game_payload);
    invalid_game_message.metadata[rpc_network::metadata_key::connection_id] = "9999";
    const auto invalid_game_response = gateway_rpc.handle(invalid_game_message);
    if (!require(invalid_game_response.status == yuan::rpc::RpcStatus::not_found, "gateway should reject invalid session id")) {
        return 24;
    }
    if (!require(invalid_game_response.metadata.find(rpc_network::metadata_key::close_connection) != invalid_game_response.metadata.end(),
                 "gateway should close connection when first business packet is not logged in")) {
        return 26;
    }

    yuan::rpc::Bytes game_payload;
    if (!require(encode_binary(CSGameRequest{player_uid,
                                                              role_id,
                                                              0,
                                                              yuan::rpc::Codec<std::string>::encode("move:plain")},
                                            game_payload),
                 "valid game request should encode")) {
        return 42;
    }
    yuan::rpc::Message game_message;
    game_message.route = game_route::gateway_game_forward();
    game_message.payload = std::move(game_payload);
    game_message.metadata[rpc_network::metadata_key::connection_id] = "1001";
    const auto game_response = gateway_rpc.handle(game_message);
    if (!require(game_response.status == yuan::rpc::RpcStatus::ok, "gateway should forward valid game request by connection context")) {
        return 43;
    }

    expected_push_session = internal_login->gateway_session_id;
    yuan::rpc::Message push_message;
    push_message.route = game_route::gateway_push();
    push_message.payload = yuan::rpc::Codec<std::string>::encode("server-event");
    push_message.metadata[game_metadata_key::gateway_session_id] = std::to_string(internal_login->gateway_session_id);
    push_message.metadata[game_metadata_key::gateway_internal_secret] = std::to_string(kDefaultLoginTokenSecret);
    const auto push_response = gateway_rpc.handle(push_message);
    if (!require(push_response.status == yuan::rpc::RpcStatus::ok && pushed_to_gateway, "gateway should accept push for active session")) {
        return 46;
    }

    yuan::rpc::Message close_message;
    close_message.route = game_route::gateway_session_close();
    close_message.metadata[game_metadata_key::gateway_session_id] = std::to_string(internal_login->gateway_session_id);
    const auto unauthorized_close_response = gateway_rpc.handle(close_message);
    if (!require(unauthorized_close_response.status == yuan::rpc::RpcStatus::bad_request, "client should not call gateway session close without internal secret")) {
        return 64;
    }
    close_message.metadata[game_metadata_key::gateway_internal_secret] = std::to_string(kDefaultLoginTokenSecret);
    const auto close_response = gateway_rpc.handle(close_message);
    if (!require(close_response.status == yuan::rpc::RpcStatus::ok && closed_gateway_session, "zone should be able to notify gateway to close session")) {
        return 52;
    }

    if (!require(!gateway.sessions.session_for_connection(1001), "gateway session close should clear local connection session")) {
        return 53;
    }

    yuan::rpc::Bytes relogin_payload;
    if (!require(encode_binary(CSLoginRequest{player_uid, role_id, valid_login_token(zone_id.pack()), 0}, relogin_payload), "relogin request should encode")) {
        return 54;
    }
    yuan::rpc::Message relogin_message;
    relogin_message.route = game_route::gateway_login();
    relogin_message.payload = std::move(relogin_payload);
    relogin_message.metadata[rpc_network::metadata_key::connection_id] = "1001";
    if (!require(gateway_rpc.handle(relogin_message).status == yuan::rpc::RpcStatus::ok, "gateway should allow login again after zone close")) {
        return 54;
    }
    const auto relogin = gateway.sessions.session_for_connection(1001);
    if (!require(relogin && relogin->zone_service_id == zone_id.pack(), "gateway relogin should restore connection session")) {
        return 55;
    }

    yuan::rpc::Bytes duplicate_role_payload;
    if (!require(encode_binary(CSLoginRequest{player_uid, role_id, valid_login_token(zone_id.pack()), 0}, duplicate_role_payload), "duplicate role login request should encode")) {
        return 60;
    }
    yuan::rpc::Message duplicate_role_message;
    duplicate_role_message.route = game_route::gateway_login();
    duplicate_role_message.payload = std::move(duplicate_role_payload);
    duplicate_role_message.metadata[rpc_network::metadata_key::connection_id] = "1002";
    const auto duplicate_role_response = gateway_rpc.handle(duplicate_role_message);
    if (!require(duplicate_role_response.status == yuan::rpc::RpcStatus::ok, "same role login from another connection should reach zone/world")) {
        return 61;
    }
    const auto duplicate_role_session = gateway.sessions.session_for_connection(1002);
    if (!require(duplicate_role_session && duplicate_role_session->gateway_session_id != relogin->gateway_session_id, "duplicate role login should allocate a new gateway session")) {
        return 62;
    }
    expected_push_session = relogin->gateway_session_id;
    closed_gateway_session = false;
    yuan::rpc::Message duplicate_close_message;
    duplicate_close_message.route = game_route::gateway_session_close();
    duplicate_close_message.metadata[game_metadata_key::gateway_session_id] = std::to_string(relogin->gateway_session_id);
    duplicate_close_message.metadata[game_metadata_key::gateway_internal_secret] = std::to_string(kDefaultLoginTokenSecret);
    const auto duplicate_close_response = gateway_rpc.handle(duplicate_close_message);
    if (!require(duplicate_close_response.status == yuan::rpc::RpcStatus::ok && closed_gateway_session && !gateway.sessions.session_for_connection(1001),
                 "zone/world duplicate-login decision should close old gateway session")) {
        return 63;
    }

    yuan::rpc::Bytes switch_login_payload;
    if (!require(encode_binary(CSLoginRequest{player_uid, new_role_id, valid_login_token(zone_2_id.pack()), 0}, switch_login_payload), "switch role login request should encode")) {
        return 47;
    }
    yuan::rpc::Message switch_login_message;
    switch_login_message.route = game_route::gateway_login();
    switch_login_message.payload = std::move(switch_login_payload);
    switch_login_message.metadata[rpc_network::metadata_key::connection_id] = "1001";
    const auto switch_login_response = gateway_rpc.handle(switch_login_message);
    if (!require(switch_login_response.status == yuan::rpc::RpcStatus::ok, "same connection role switch login should succeed")) {
        return 48;
    }
    const auto internal_switch_login = gateway.sessions.session_for_connection(1001);
    if (!require(internal_switch_login && internal_switch_login->gateway_session_id != internal_login->gateway_session_id && internal_switch_login->zone_service_id == zone_2_id.pack(),
                 "gateway should replace connection session during role switch")) {
        return 49;
    }
    if (!require(world_player_zone(world, role_id).value_or(0) == 0, "old role should be logged out when same connection switches role")) {
        return 50;
    }
    if (!require(world_player_zone(world, new_role_id).value_or(0) == zone_2_id.pack(), "new role should be online after same connection switch")) {
        return 51;
    }
    if (!require(world.online_by_uid[player_uid].role_id == new_role_id && world.online_by_role.contains(new_role_id) && !world.online_by_role.contains(role_id),
                 "world should keep exactly one online role per uid")) {
        return 67;
    }
    if (!require(world_set_player_zone(world, role_id, 0, zone_id.pack(), relogin->gateway_session_id),
                 "stale logout from old same-uid role should be ignored")) {
        return 68;
    }
    if (!require(world_player_zone(world, new_role_id).value_or(0) == zone_2_id.pack() && world.online_by_uid[player_uid].role_id == new_role_id,
                 "stale old-role logout must not clear current uid session")) {
        return 69;
    }

    yuan::rpc::Bytes new_login_payload;
    if (!require(encode_binary(CSLoginRequest{player_uid, new_role_id, valid_login_token(zone_2_id.pack()), 0}, new_login_payload), "new role login request should encode")) {
        return 14;
    }
    yuan::rpc::Message new_login_message;
    new_login_message.route = game_route::gateway_login();
    new_login_message.payload = std::move(new_login_payload);
    new_login_message.metadata[rpc_network::metadata_key::connection_id] = "1002";
    const auto new_login_response = gateway_rpc.handle(new_login_message);
    if (!require(new_login_response.status == yuan::rpc::RpcStatus::ok, "new role gateway login should succeed")) {
        return 15;
    }
    const auto new_login = decode_binary<CSLoginResponse>(new_login_response.payload);
    if (!require(new_login && new_login->ok && new_login->role_id == new_role_id && new_login->zone_service_id == 0 && new_login->gateway_session_id == 0,
                 "new role client response should hide internals")) {
        return 16;
    }
    const auto internal_new_login = gateway.sessions.session_for_connection(1002);
    if (!require(internal_new_login && internal_new_login->gateway_session_id != 0 && internal_new_login->gateway_session_id != internal_login->gateway_session_id,
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
    world_add_role(world, burst_player_uid_1, SSPlayerRoleInfo{burst_role_1, "rogue", 1, world_id.pack(), 0});
    world_add_role(world, burst_player_uid_2, SSPlayerRoleInfo{burst_role_2, "priest", 1, world_id.pack(), 0});
    world_register_zone(world, SSZoneInfo{zone_id.pack(), "zone-1", 0, 1, true, {SSGatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"}}});
    world_register_zone(world, SSZoneInfo{zone_2_id.pack(), "zone-2", 0, 1, true, {SSGatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"}}});
    const auto first_burst_options = world_login_options(world, burst_player_uid_1);
    const auto second_burst_options = world_login_options(world, burst_player_uid_2);
    const auto duplicate_first_burst_options = world_login_options(world, burst_player_uid_1);
    LoginTokenId first_burst_token;
    LoginTokenId second_burst_token;
    for (const auto &role : first_burst_options.roles) {
        if (role.role_id == burst_role_1) {
            first_burst_token = role.login_token_id;
        }
    }
    for (const auto &role : second_burst_options.roles) {
        if (role.role_id == burst_role_2) {
            second_burst_token = role.login_token_id;
        }
    }
    if (!require(!first_burst_token.empty() && !second_burst_token.empty() && decode_login_token_id(first_burst_token, yuan::base::time::steady_now_ms(), kDefaultLoginTokenSecret) != decode_login_token_id(second_burst_token, yuan::base::time::steady_now_ms(), kDefaultLoginTokenSecret),
                 "pending reservations should spread burst login options across capacity")) {
        return 28;
    }
    LoginTokenId duplicate_first_burst_token;
    for (const auto &role : duplicate_first_burst_options.roles) {
        if (role.role_id == burst_role_1) {
            duplicate_first_burst_token = role.login_token_id;
        }
    }
    if (!require(duplicate_first_burst_token == first_burst_token, "duplicate login options should refresh the same role reservation")) {
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

    world_register_zone(world, SSZoneInfo{zone_id.pack(), "zone-1", 0, 100, true, {SSGatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"}}});
    world_register_zone(world, SSZoneInfo{zone_2_id.pack(), "zone-2", 3, 100, true, {SSGatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"}}});
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
    world_register_zone(world, SSZoneInfo{zone_id.pack(), "zone-1", 0, 100, true, {SSGatewayInfo{gateway_id.pack(), "127.0.0.1", 30001, "gateway-a"}}});
    const auto fresh_zone = world_select_zone(world, player_uid, new_role_id);
    if (!require(fresh_zone && *fresh_zone == zone_id.pack(), "fresh zone report should make zone selectable again")) {
        return 39;
    }

    yuan::base::time::reset_test_time();
    return EXIT_SUCCESS;
}
