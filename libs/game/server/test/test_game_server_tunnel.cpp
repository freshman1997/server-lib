#include "game_server.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

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

    GlobalService global(ServiceAddress{{1, 1, GameServiceType::global, 1, 1}, 100, yuan::game_base::ServerRole::world, 1, "global"});
    ZoneService zone_1(ServiceAddress{{1, 1, GameServiceType::zone, 1, 1}, 200, yuan::game_base::ServerRole::scene, 1, "zone-1"}, tunnels);
    ZoneService zone_2(ServiceAddress{{1, 1, GameServiceType::zone, 1, 2}, 201, yuan::game_base::ServerRole::scene, 1, "zone-2"}, tunnels);
    if (!require(global.address().service.pack() == pack_game_service_id(1, 1, GameServiceType::global, 1),
                 "packed global service id should match layout")) {
        return 29;
    }
    if (!require(unpack_game_service_id(zone_2.address().service.pack()) == zone_2.address().service,
                 "packed zone service id should roundtrip")) {
        return 30;
    }

    if (!require(tunnels.register_endpoint(global.address(), global.rpc_server()), "global should register on tunnels")) {
        return 2;
    }
    if (!require(tunnels.register_endpoint(zone_1.address(), zone_1.rpc_server()), "zone-1 should register on tunnels")) {
        return 3;
    }
    if (!require(tunnels.register_endpoint(zone_2.address(), zone_2.rpc_server()), "zone-2 should register on tunnels")) {
        return 4;
    }
    if (!require(tunnel_a->endpoint_count() == 3 && tunnel_b->endpoint_count() == 3, "all tunnel instances should know endpoints")) {
        return 5;
    }

    yuan::rpc::Route global_route;
    global_route.name = std::string(route::global_echo);
    auto response = zone_1.call(global.address(), global_route, yuan::rpc::Codec<std::string>::encode("hello-global"));
    if (!require(response.status == yuan::rpc::RpcStatus::ok, "zone to global through tunnel should succeed")) {
        return 6;
    }
    if (!require(yuan::rpc::Codec<std::string>::decode(response.payload) == "hello-global", "global echo payload mismatch")) {
        return 7;
    }
    if (!require(response.metadata.find("global.node") != response.metadata.end(), "global metadata should be present")) {
        return 8;
    }
    if (!require(response.metadata["tunnel.source"] == service_key(zone_1.address()), "global should know tunnel source")) {
        return 13;
    }
    if (!require(response.metadata["tunnel.target"] == service_key(global.address()), "global should know tunnel target")) {
        return 14;
    }

    yuan::rpc::Route zone_route;
    zone_route.name = std::string(route::zone_echo);
    response = zone_2.call(zone_1.address(), zone_route, yuan::rpc::Codec<std::string>::encode("hello-zone"), {}, 77, 8801);
    if (!require(response.status == yuan::rpc::RpcStatus::ok, "zone to zone through tunnel should succeed")) {
        return 9;
    }
    if (!require(response.metadata.find("zone.node") != response.metadata.end(), "zone metadata should be present")) {
        return 10;
    }
    if (!require(response.metadata["tunnel.source"] == service_key(zone_2.address()), "target zone should know source zone")) {
        return 15;
    }
    if (!require(response.metadata["tunnel.origin.request_id"] == "77", "target zone should receive origin request id")) {
        return 16;
    }
    if (!require(response.metadata["tunnel.origin.continuation_id"] == "8801", "target zone should receive origin continuation id")) {
        return 17;
    }
    if (!require(response.request_id == 77, "response should preserve origin request id")) {
        return 18;
    }
    if (!require(response.continuation_id() == 8801, "response should preserve origin continuation id")) {
        return 19;
    }

    TunnelEnvelope random_zone_envelope;
    random_zone_envelope.source = service_key(global.address());
    random_zone_envelope.source_service_id = global.address().service.pack();
    random_zone_envelope.target_type = GameServiceType::zone;
    random_zone_envelope.mode = TunnelEnvelope::ForwardMode::random_one;
    random_zone_envelope.request_id = 301;
    random_zone_envelope.continuation_id = 9301;
    random_zone_envelope.route = zone_route;
    random_zone_envelope.payload = yuan::rpc::Codec<std::string>::encode("random-zone");
    auto random_response = tunnel_a->forward(random_zone_envelope);
    if (!require(random_response.status == yuan::rpc::RpcStatus::ok, "random zone forward should succeed")) {
        return 35;
    }
    if (!require(random_response.metadata.find("zone.node") != random_response.metadata.end(), "random zone should hit a zone")) {
        return 36;
    }

    TunnelEnvelope broadcast_zone_envelope;
    broadcast_zone_envelope.source = service_key(global.address());
    broadcast_zone_envelope.source_service_id = global.address().service.pack();
    broadcast_zone_envelope.target_type = GameServiceType::zone;
    broadcast_zone_envelope.mode = TunnelEnvelope::ForwardMode::all_of_type;
    broadcast_zone_envelope.request_id = 302;
    broadcast_zone_envelope.continuation_id = 9302;
    broadcast_zone_envelope.route = zone_route;
    broadcast_zone_envelope.payload = yuan::rpc::Codec<std::string>::encode("broadcast-zone");
    auto broadcast_response = tunnel_a->forward(broadcast_zone_envelope);
    if (!require(broadcast_response.status == yuan::rpc::RpcStatus::ok, "broadcast zone forward should succeed")) {
        return 37;
    }
    if (!require(broadcast_response.metadata["tunnel.broadcast.count"] == "2" && broadcast_response.metadata["tunnel.broadcast.ok"] == "2",
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
    async_envelope.source = service_key(zone_1.address());
    async_envelope.target = service_key(async_global_address);
    async_envelope.source_service_id = zone_1.address().service.pack();
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
    if (!require(captured_async_message.metadata["tunnel.source"] == service_key(zone_1.address()), "async target should know source")) {
        return 24;
    }

    TunnelReply async_reply;
    async_reply.source = service_key(async_global_address);
    async_reply.target = service_key(zone_1.address());
    async_reply.source_service_id = async_global_address.service.pack();
    async_reply.target_service_id = zone_1.address().service.pack();
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
    rpc_reply_envelope.source = service_key(zone_2.address());
    rpc_reply_envelope.target = service_key(async_global_address);
    rpc_reply_envelope.source_service_id = zone_2.address().service.pack();
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
    rpc_reply.target = service_key(zone_2.address());
    rpc_reply.source_service_id = async_global_address.service.pack();
    rpc_reply.target_service_id = zone_2.address().service.pack();
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
    tunnel_reply_message.route.name = std::string(route::tunnel_reply);
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

    response = zone_1.call(ServiceAddress{{1, 1, GameServiceType::zone, 1, 999}, 999, yuan::game_base::ServerRole::scene, 1, "missing"}, zone_route, {});
    if (!require(response.status == yuan::rpc::RpcStatus::not_found, "missing tunnel target should fail")) {
        return 11;
    }

    if (!require(global.request_count() == 1, "global should receive exactly one request")) {
        return 12;
    }
    return EXIT_SUCCESS;
}
