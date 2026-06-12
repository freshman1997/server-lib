#include "game_server.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

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

    TunnelService tunnel(ServiceAddress{{1, 1, GameServiceType::tunnel, 1, 1}, 1, yuan::game_base::ServerRole::gateway, 1, "tunnel"});
    GlobalService global(ServiceAddress{{1, 1, GameServiceType::global, 1, 1}, 100, yuan::game_base::ServerRole::world, 1, "global"});
    if (!require(tunnel.register_endpoint(global.address(), global.rpc_server()), "global should register on tunnel")) {
        return 1;
    }

    const auto port = rpc_network::reserve_loopback_port();
    if (!require(port != 0, "server should reserve a loopback port")) {
        return 2;
    }

    std::atomic_bool server_ok{true};
    std::thread server_thread([&] {
        try {
            rpc_network::RpcNetworkServer server;
            server_ok = server.bind_loopback(port, tunnel.rpc_server(), 1) && server.run();
        } catch (...) {
            server_ok = false;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    TunnelEnvelope envelope;
    const GameServiceId zone_service{1, 1, GameServiceType::zone, 1, 1};
    envelope.source_service_id = zone_service.pack();
    envelope.target_service_id = global.address().service.pack();
    envelope.source = service_id_key(zone_service);
    envelope.target = service_key(global.address());
    envelope.route = game_route::global_echo();
    envelope.payload = yuan::rpc::Codec<std::string>::encode("hello-core-global");
    envelope.metadata["trace_id"] = "network-smoke";

    yuan::rpc::Bytes envelope_payload;
    if (!require(encode_tunnel_envelope(envelope, envelope_payload), "tunnel envelope should encode")) {
        server_thread.join();
        return 3;
    }

    yuan::rpc::Message message;
    message.kind = yuan::rpc::MessageKind::request;
    message.request_id = 42;
    message.route = game_route::tunnel_forward();
    message.payload = std::move(envelope_payload);

    const auto response = rpc_network::RpcNetworkClient().call(rpc_network::RpcEndpoint{"127.0.0.1", port}, message);
    if (!require(response.has_value(), "client should receive response through Core network")) {
        server_thread.join();
        return 4;
    }
    if (!require(response->status == yuan::rpc::RpcStatus::ok, "Core network tunnel forward should succeed")) {
        server_thread.join();
        return 5;
    }
    if (!require(yuan::rpc::Codec<std::string>::decode(response->payload) == "hello-core-global", "global Core network payload mismatch")) {
        server_thread.join();
        return 6;
    }
    if (!require(response->metadata.find("global.node") != response->metadata.end(), "global metadata should return over Core network")) {
        server_thread.join();
        return 7;
    }

    server_thread.join();
    if (!require(server_ok.load(), "Core network server should complete successfully")) {
        return 8;
    }

    return EXIT_SUCCESS;
}
