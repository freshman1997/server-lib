#include "common/codec/game_binary_codec.h"
#include "common/game_rpc_protocol.h"
#include "common/metadata_keys.h"
#include "common/proto/client_proto.h"
#include "global/rpc/global_msg_echo.h"
#include "global/rpc/global_msg_gm.h"
#include "messaging/tunnel_client_manager.h"
#include "tunnel/rpc/tunnel_service.h"
#include "zone/rpc/zone_msg_echo.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>

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
    auto tunnel_a = std::make_shared<TunnelService>(ServiceAddress{{1, 1, GameServiceType::tunnel, 1, 1}, 1, yuan::game_base::ServerRole::gateway, 1, "tunnel-a"});
    auto tunnel_b = std::make_shared<TunnelService>(ServiceAddress{{1, 1, GameServiceType::tunnel, 1, 2}, 2, yuan::game_base::ServerRole::gateway, 1, "tunnel-b"});
    if (!require(tunnels.add(tunnel_a) && tunnels.add(tunnel_b), "tunnel instances should be added")) {
        return 1;
    }

    const ServiceAddress global_address{{1, 1, GameServiceType::global, 1, 1}, 100, yuan::game_base::ServerRole::world, 1, "global"};
    GlobalMsgEchoContext global_echo{global_address};
    GlobalMsgGmContext global_gm;
    yuan::rpc::Server global_rpc;
    register_global_builtin_gm(global_gm);
    if (!require(register_global_msg_echo(global_rpc, global_echo) && register_global_msg_gm(global_rpc, global_gm),
                 "global handlers should register")) {
        return 40;
    }
    const ServiceAddress zone_1_address{{1, 1, GameServiceType::zone, 1, 1}, 200, yuan::game_base::ServerRole::scene, 1, "zone-1"};
    const ServiceAddress zone_2_address{{1, 1, GameServiceType::zone, 1, 2}, 201, yuan::game_base::ServerRole::scene, 1, "zone-2"};
    yuan::rpc::Server zone_1_rpc;
    yuan::rpc::Server zone_2_rpc;
    if (!require(register_zone_msg_echo(zone_1_rpc, zone_1_address) && register_zone_msg_echo(zone_2_rpc, zone_2_address),
                 "zone echo handlers should register")) {
        return 39;
    }
    if (!require(global_address.service.pack() == pack_game_service_id(1, 1, GameServiceType::global, 1),
                 "packed global service id should match layout")) {
        return 29;
    }
    if (!require(unpack_game_service_id(zone_2_address.service.pack()) == zone_2_address.service,
                 "packed zone service id should roundtrip")) {
        return 30;
    }

    if (!require(tunnels.register_endpoint(global_address, global_rpc), "global should register on tunnels")) {
        return 2;
    }
    if (!require(tunnels.register_endpoint(zone_1_address, zone_1_rpc), "zone-1 should register on tunnels")) {
        return 3;
    }
    if (!require(tunnels.register_endpoint(zone_2_address, zone_2_rpc), "zone-2 should register on tunnels")) {
        return 4;
    }
    if (!require(tunnel_a->endpoint_count() == 3 && tunnel_b->endpoint_count() == 3, "all tunnel instances should know endpoints")) {
        return 5;
    }

    yuan::rpc::Route global_route = game_route::global_echo();
    TunnelEnvelope zone_to_global;
    zone_to_global.source = service_key(zone_1_address);
    zone_to_global.target = service_key(global_address);
    zone_to_global.source_service_id = zone_1_address.service.pack();
    zone_to_global.target_service_id = global_address.service.pack();
    zone_to_global.route = global_route;
    zone_to_global.payload = yuan::rpc::Codec<std::string>::encode("hello-global");
    auto response = tunnels.forward(std::move(zone_to_global));
    if (!require(response.status == yuan::rpc::RpcStatus::ok, "zone to global through tunnel should succeed")) {
        return 6;
    }
    if (!require(yuan::rpc::Codec<std::string>::decode(response.payload) == "hello-global", "global echo payload mismatch")) {
        return 7;
    }
    if (!require(response.metadata.find(game_metadata_key::global_node) != response.metadata.end(), "global metadata should be present")) {
        return 8;
    }
    if (!require(response.metadata[game_metadata_key::tunnel_source] == service_key(zone_1_address), "global should know tunnel source")) {
        return 13;
    }
    if (!require(response.metadata[game_metadata_key::tunnel_target] == service_key(global_address), "global should know tunnel target")) {
        return 14;
    }

    yuan::rpc::Route zone_route = game_route::zone_echo();
    TunnelEnvelope zone_to_zone;
    zone_to_zone.source = service_key(zone_2_address);
    zone_to_zone.target = service_key(zone_1_address);
    zone_to_zone.source_service_id = zone_2_address.service.pack();
    zone_to_zone.target_service_id = zone_1_address.service.pack();
    zone_to_zone.request_id = 77;
    zone_to_zone.continuation_id = 8801;
    zone_to_zone.route = zone_route;
    (void)encode_binary(CSGameRequest{1, 10001, 0, yuan::rpc::Codec<std::string>::encode("hello-zone")}, zone_to_zone.payload);
    zone_to_zone.metadata[game_metadata_key::gateway_session_id] = "1";
    response = tunnels.forward(std::move(zone_to_zone));
    if (!require(response.status == yuan::rpc::RpcStatus::ok, "zone to zone through tunnel should succeed")) {
        return 9;
    }
    if (!require(response.metadata.find(game_metadata_key::zone_node) != response.metadata.end(), "zone metadata should be present")) {
        return 10;
    }
    if (!require(response.metadata[game_metadata_key::tunnel_source] == service_key(zone_2_address), "target zone should know source zone")) {
        return 15;
    }
    if (!require(response.metadata[game_metadata_key::tunnel_origin_request_id] == "77", "target zone should receive origin request id")) {
        return 16;
    }
    if (!require(response.metadata[game_metadata_key::tunnel_origin_continuation_id] == "8801", "target zone should receive origin continuation id")) {
        return 17;
    }
    if (!require(response.request_id == 77, "response should preserve origin request id")) {
        return 18;
    }
    if (!require(response.continuation_id() == 8801, "response should preserve origin continuation id")) {
        return 19;
    }

    TunnelEnvelope random_zone_envelope;
    random_zone_envelope.source = service_key(global_address);
    random_zone_envelope.source_service_id = global_address.service.pack();
    random_zone_envelope.target_type = GameServiceType::zone;
    random_zone_envelope.mode = TunnelEnvelope::ForwardMode::random_one;
    random_zone_envelope.request_id = 301;
    random_zone_envelope.continuation_id = 9301;
    random_zone_envelope.route = zone_route;
    (void)encode_binary(CSGameRequest{1, 10001, 0, yuan::rpc::Codec<std::string>::encode("random-zone")}, random_zone_envelope.payload);
    random_zone_envelope.metadata[game_metadata_key::gateway_session_id] = "1";
    auto random_response = tunnel_a->forward(random_zone_envelope);
    if (!require(random_response.status == yuan::rpc::RpcStatus::ok, "random zone forward should succeed")) {
        return 35;
    }
    if (!require(random_response.metadata.find(game_metadata_key::zone_node) != random_response.metadata.end(), "random zone should hit a zone")) {
        return 36;
    }

    TunnelEnvelope broadcast_zone_envelope;
    broadcast_zone_envelope.source = service_key(global_address);
    broadcast_zone_envelope.source_service_id = global_address.service.pack();
    broadcast_zone_envelope.target_type = GameServiceType::zone;
    broadcast_zone_envelope.mode = TunnelEnvelope::ForwardMode::all_of_type;
    broadcast_zone_envelope.request_id = 302;
    broadcast_zone_envelope.continuation_id = 9302;
    broadcast_zone_envelope.route = zone_route;
    (void)encode_binary(CSGameRequest{1, 10001, 0, yuan::rpc::Codec<std::string>::encode("broadcast-zone")}, broadcast_zone_envelope.payload);
    broadcast_zone_envelope.metadata[game_metadata_key::gateway_session_id] = "1";
    auto broadcast_response = tunnel_a->forward(broadcast_zone_envelope);
    if (!require(broadcast_response.status == yuan::rpc::RpcStatus::ok, "broadcast zone forward should succeed")) {
        return 37;
    }
    if (!require(broadcast_response.metadata[game_metadata_key::tunnel_broadcast_count] == "2" && broadcast_response.metadata[game_metadata_key::tunnel_broadcast_ok] == "2",
                 "broadcast should reach all zone instances")) {
        return 38;
    }

    ServiceAddress async_global_address{{1, 1, GameServiceType::global, 1, 2}, 101, yuan::game_base::ServerRole::world, 1, "async-global"};
    yuan::rpc::Message captured_async_message;
    if (!require(tunnel_a->register_endpoint_handler(async_global_address, [&](yuan::rpc::Message message) {
            captured_async_message = std::move(message);
            yuan::rpc::Response accepted;
            accepted.request_id = captured_async_message.request_id;
            accepted.set_continuation_id(captured_async_message.continuation_id());
            accepted.status = yuan::rpc::RpcStatus::ok;
            return accepted;
        }), "async endpoint should register")) {
        return 20;
    }

    TunnelEnvelope async_envelope;
    async_envelope.source = service_key(zone_1_address);
    async_envelope.target = service_key(async_global_address);
    async_envelope.source_service_id = zone_1_address.service.pack();
    async_envelope.target_service_id = async_global_address.service.pack();
    async_envelope.request_id = 501;
    async_envelope.continuation_id = 9501;
    async_envelope.route = global_route;
    async_envelope.payload = yuan::rpc::Codec<std::string>::encode("async-request");

    bool async_replied = false;
    yuan::rpc::Response async_response;
    if (!require(tunnel_a->forward_async(async_envelope, [&](yuan::rpc::Response reply) {
            async_replied = true;
            async_response = std::move(reply);
        }), "async forward should register pending continuation")) {
        return 21;
    }
    if (!require(!async_replied && tunnel_a->pending_reply_count() == 1, "async reply should remain pending")) {
        return 22;
    }
    if (!require(captured_async_message.request_id == 501 && captured_async_message.continuation_id() == 9501,
                 "async target should receive request and continuation ids")) {
        return 23;
    }
    if (!require(captured_async_message.metadata[game_metadata_key::tunnel_source] == service_key(zone_1_address), "async target should know source")) {
        return 24;
    }

    TunnelReply async_reply;
    async_reply.source = service_key(async_global_address);
    async_reply.target = service_key(zone_1_address);
    async_reply.source_service_id = async_global_address.service.pack();
    async_reply.target_service_id = zone_1_address.service.pack();
    async_reply.request_id = 501;
    async_reply.continuation_id = 9501;
    async_reply.status = yuan::rpc::RpcStatus::ok;
    async_reply.payload = yuan::rpc::Codec<std::string>::encode("async-response");
    async_reply.metadata["async.done"] = "true";
    const auto async_ack = tunnel_a->handle_reply(std::move(async_reply));
    if (!require(async_ack.status == yuan::rpc::RpcStatus::ok, "async reply should ack")) {
        return 25;
    }
    if (!require(async_replied && tunnel_a->pending_reply_count() == 0, "async reply should resume pending callback")) {
        return 26;
    }
    if (!require(async_response.request_id == 501 && async_response.continuation_id() == 9501,
                 "async response should preserve continuation")) {
        return 27;
    }
    if (!require(yuan::rpc::Codec<std::string>::decode(async_response.payload) == "async-response", "async response payload mismatch")) {
        return 28;
    }

    bool rpc_reply_replied = false;
    yuan::rpc::Response rpc_reply_response;
    TunnelEnvelope rpc_reply_envelope;
    rpc_reply_envelope.source = service_key(zone_2_address);
    rpc_reply_envelope.target = service_key(async_global_address);
    rpc_reply_envelope.source_service_id = zone_2_address.service.pack();
    rpc_reply_envelope.target_service_id = async_global_address.service.pack();
    rpc_reply_envelope.request_id = 502;
    rpc_reply_envelope.continuation_id = 9502;
    rpc_reply_envelope.route = global_route;
    rpc_reply_envelope.payload = yuan::rpc::Codec<std::string>::encode("rpc-reply-request");
    if (!require(tunnel_a->forward_async(rpc_reply_envelope, [&](yuan::rpc::Response reply) {
            rpc_reply_replied = true;
            rpc_reply_response = std::move(reply);
        }), "rpc reply forward should register pending continuation")) {
        return 31;
    }

    TunnelReply rpc_reply;
    rpc_reply.source = service_key(async_global_address);
    rpc_reply.target = service_key(zone_2_address);
    rpc_reply.source_service_id = async_global_address.service.pack();
    rpc_reply.target_service_id = zone_2_address.service.pack();
    rpc_reply.request_id = 502;
    rpc_reply.continuation_id = 9502;
    rpc_reply.payload = yuan::rpc::Codec<std::string>::encode("rpc-reply-response");
    yuan::rpc::Bytes rpc_reply_payload;
    if (!require(encode_tunnel_reply(rpc_reply, rpc_reply_payload), "tunnel reply should encode")) {
        return 32;
    }
    yuan::rpc::Message tunnel_reply_message;
    tunnel_reply_message.kind = yuan::rpc::MessageKind::request;
    tunnel_reply_message.request_id = 502;
    tunnel_reply_message.set_continuation_id(9502);
    tunnel_reply_message.route = game_route::tunnel_reply();
    tunnel_reply_message.payload = std::move(rpc_reply_payload);
    const auto rpc_reply_ack = tunnel_a->rpc_server().handle(tunnel_reply_message);
    if (!require(rpc_reply_ack.status == yuan::rpc::RpcStatus::ok && rpc_reply_replied,
                 "tunnel.reply rpc handler should resume pending callback")) {
        return 33;
    }
    if (!require(rpc_reply_response.request_id == 502 && rpc_reply_response.continuation_id() == 9502,
                 "tunnel.reply rpc response should preserve continuation")) {
        return 34;
    }

    TunnelEnvelope missing_envelope;
    missing_envelope.source = service_key(zone_1_address);
    missing_envelope.target = service_key(ServiceAddress{{1, 1, GameServiceType::zone, 1, 999}, 999, yuan::game_base::ServerRole::scene, 1, "missing"});
    missing_envelope.source_service_id = zone_1_address.service.pack();
    missing_envelope.target_service_id = pack_game_service_id(1, 1, GameServiceType::zone, 999);
    missing_envelope.route = zone_route;
    response = tunnels.forward(std::move(missing_envelope));
    if (!require(response.status == yuan::rpc::RpcStatus::not_found, "missing tunnel target should fail")) {
        return 11;
    }

    const auto retry_tunnel_port = rpc_network::reserve_loopback_port();
    if (!require(retry_tunnel_port != 0, "retry tunnel port should be reserved")) {
        return 40;
    }
    rpc_network::RpcNetworkServer retry_tunnel_server;
    if (!require(retry_tunnel_server.start(rpc_network::RpcNetworkServerConfig{"127.0.0.1", retry_tunnel_port, 0}, tunnel_a->rpc_server()),
                 "retry tunnel network server should start")) {
        return 41;
    }
    std::thread retry_tunnel_thread([&retry_tunnel_server] {
        (void)retry_tunnel_server.run();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    TunnelClientManager retry_manager({rpc_network::RpcEndpoint{"127.0.0.1", 1}, rpc_network::RpcEndpoint{"127.0.0.1", retry_tunnel_port}});
    auto retry_response = retry_manager.send_to_service(zone_1_address.service.pack(),
                                                         global_address.service.pack(),
                                                         game_route::global_echo(),
                                                         yuan::rpc::Codec<std::string>::encode("retry-global"));
    if (!require(retry_response && retry_response->status == yuan::rpc::RpcStatus::ok,
                 "process message manager should retry next tunnel endpoint")) {
        return 42;
    }
    if (!require(yuan::rpc::Codec<std::string>::decode(retry_response->payload) == "retry-global",
                  "retry tunnel response payload mismatch")) {
        return 43;
    }
    TunnelEnvelope retry_metrics_envelope;
    retry_metrics_envelope.source = service_key(zone_1_address);
    retry_metrics_envelope.target = service_key(global_address);
    retry_metrics_envelope.source_service_id = zone_1_address.service.pack();
    retry_metrics_envelope.target_service_id = global_address.service.pack();
    retry_metrics_envelope.route = game_route::global_echo();
    retry_metrics_envelope.payload = yuan::rpc::Codec<std::string>::encode("retry-metrics-global");
    yuan::rpc::Bytes retry_metrics_payload;
    if (!require(encode_tunnel_envelope(retry_metrics_envelope, retry_metrics_payload), "retry metrics tunnel envelope should encode")) {
        return 44;
    }
    yuan::rpc::Message retry_metrics_message;
    retry_metrics_message.route = game_route::tunnel_forward();
    retry_metrics_message.payload = std::move(retry_metrics_payload);
    const auto retry_metrics_response = retry_manager.call_tunnel(std::move(retry_metrics_message), 0);
    if (!require(retry_metrics_response && retry_metrics_response->status == yuan::rpc::RpcStatus::ok,
                 "process message manager should recover after retrying a dead first endpoint")) {
        return 51;
    }
    const auto retry_metrics = retry_manager.metrics();
    if (!require(retry_metrics.tunnel_call_retries >= 1 && retry_metrics.tunnel_call_recoveries >= 1,
                 "process message manager should record retry and recovery metrics")) {
        return 52;
    }
    retry_tunnel_server.stop();
    if (retry_tunnel_thread.joinable()) {
        retry_tunnel_thread.join();
    }

    const auto recovery_tunnel_port = rpc_network::reserve_loopback_port();
    if (!require(recovery_tunnel_port != 0, "recovery tunnel port should be reserved")) {
        return 45;
    }
    TunnelClientManager recovery_manager({rpc_network::RpcEndpoint{"127.0.0.1", recovery_tunnel_port}});
    auto down_response = recovery_manager.send_to_service(zone_1_address.service.pack(),
                                                          global_address.service.pack(),
                                                          game_route::global_echo(),
                                                          yuan::rpc::Codec<std::string>::encode("recovery-down"));
    if (!require(!down_response, "down tunnel endpoint should fail before server starts")) {
        return 46;
    }
    rpc_network::RpcNetworkServer recovery_tunnel_server;
    if (!require(recovery_tunnel_server.start(rpc_network::RpcNetworkServerConfig{"127.0.0.1", recovery_tunnel_port, 0}, tunnel_a->rpc_server()),
                 "recovery tunnel network server should start")) {
        return 47;
    }
    std::thread recovery_tunnel_thread([&recovery_tunnel_server] {
        (void)recovery_tunnel_server.run();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto recovered_response = recovery_manager.send_to_service(zone_1_address.service.pack(),
                                                               global_address.service.pack(),
                                                               game_route::global_echo(),
                                                               yuan::rpc::Codec<std::string>::encode("recovery-up"));
    recovery_tunnel_server.stop();
    if (recovery_tunnel_thread.joinable()) {
        recovery_tunnel_thread.join();
    }
    if (!require(recovered_response && recovered_response->status == yuan::rpc::RpcStatus::ok,
                 "process message manager should reconnect to a previously dead tunnel endpoint")) {
        return 48;
    }
    if (!require(yuan::rpc::Codec<std::string>::decode(recovered_response->payload) == "recovery-up",
                 "recovered tunnel response payload mismatch")) {
        return 49;
    }
    const auto recovery_metrics = recovery_manager.metrics();
    if (!require(recovery_metrics.tunnel_call_failures >= 1,
                 "process message manager should record failed down-endpoint calls")) {
        return 50;
    }

    if (!require(global_echo.request_count == 4, "global should receive direct, retried, metrics, and recovered requests")) {
        return 12;
    }
    return EXIT_SUCCESS;
}
