#include "game_server.h"

#include "coroutine/sync_wait.h"
#include "net/async/async_request_client.h"

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

    yuan::buffer::ByteBuffer to_buffer(const yuan::rpc::Bytes &bytes)
    {
        yuan::buffer::ByteBuffer buffer(bytes.size());
        if (!bytes.empty()) {
            buffer.append(bytes.data(), bytes.size());
        }
        return buffer;
    }

    yuan::rpc::Bytes to_bytes(const yuan::buffer::ByteBuffer &buffer)
    {
        const auto span = buffer.readable_span();
        const auto *data = reinterpret_cast<const std::uint8_t *>(span.data());
        return yuan::rpc::Bytes(data, data + span.size());
    }
}

int main()
{
    using namespace yuan::game::server;

    TunnelService tunnel(ServiceAddress{{1, 1, GameServiceType::tunnel, 1, 1}, 1, yuan::game_base::ServerRole::gateway, 1, "tunnel"});
    const ServiceAddress global_address{{1, 1, GameServiceType::global, 1, 1}, 100, yuan::game_base::ServerRole::world, 1, "global"};
    GlobalMsgEchoContext global_echo{global_address};
    yuan::rpc::Server global_rpc;
    if (!require(register_global_msg_echo(global_rpc, global_echo), "global echo handler should register")) {
        return 9;
    }
    if (!require(tunnel.register_endpoint(global_address, global_rpc), "global should register on tunnel")) {
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
            server_ok = server.bind_loopback(port, tunnel.rpc_server(), 2) && server.run();
        } catch (...) {
            server_ok = false;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    TunnelEnvelope envelope;
    const GameServiceId zone_service{1, 1, GameServiceType::zone, 1, 1};
    envelope.source_service_id = zone_service.pack();
    envelope.target_service_id = global_address.service.pack();
    envelope.source = service_id_key(zone_service);
    envelope.target = service_key(global_address);
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

    const auto persistent_ok = [&]() {
        yuan::net::NetworkRuntime client_runtime;
        auto rv = client_runtime.runtime_view();
        auto task = [&](yuan::coroutine::RuntimeView view) -> yuan::coroutine::Task<bool> {
            co_await view.schedule();
            yuan::net::AsyncRequestClient client(view);
            if (!co_await client.connect_async("127.0.0.1", port, 1000)) {
                co_return false;
            }

            for (std::uint64_t request_id : {42ULL, 43ULL}) {
                message.request_id = request_id;
                yuan::rpc::Bytes request_frame;
                if (!yuan::rpc::wire::encode_message(message, request_frame)) {
                    co_return false;
                }
                const auto read_result = co_await client.request_async(to_buffer(request_frame), 1000);
                if (read_result.status != yuan::coroutine::IoStatus::success) {
                    co_return false;
                }
                const auto decoded = yuan::rpc::wire::decode_frame(to_bytes(read_result.data));
                if (!decoded.ok) {
                    co_return false;
                }
                const auto response = yuan::rpc::wire::to_response(decoded.frame);
                if (response.request_id != request_id || response.status != yuan::rpc::RpcStatus::ok ||
                    yuan::rpc::Codec<std::string>::decode(response.payload) != "hello-core-global" ||
                    response.metadata.find("global.node") == response.metadata.end()) {
                    co_return false;
                }
            }
            client.disconnect();
            co_return true;
        };
        return yuan::coroutine::sync_wait(rv, task(rv));
    }();

    if (!require(persistent_ok, "persistent Core RPC connection should handle two requests")) {
        server_thread.join();
        return 4;
    }

    server_thread.join();
    if (!require(server_ok.load(), "Core network server should complete successfully")) {
        return 8;
    }

    const auto drain_port = rpc_network::reserve_loopback_port();
    if (!require(drain_port != 0, "drain server should reserve a loopback port")) {
        return 10;
    }
    rpc_network::RpcNetworkServer drain_server;
    if (!require(drain_server.start(rpc_network::RpcNetworkServerConfig{"127.0.0.1", drain_port, 0}, tunnel.rpc_server()),
                 "drain server should start")) {
        return 11;
    }
    std::jthread drain_thread([&] {
        (void)drain_server.run();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto drain_ok = [&]() {
        yuan::net::NetworkRuntime client_runtime;
        auto rv = client_runtime.runtime_view();
        auto task = [&](yuan::coroutine::RuntimeView view) -> yuan::coroutine::Task<bool> {
            co_await view.schedule();
            yuan::net::AsyncRequestClient client(view);
            if (!co_await client.connect_async("127.0.0.1", drain_port, 1000)) {
                co_return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (drain_server.active_connection_count() == 0) {
                co_return false;
            }
            drain_server.close_all_connections();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            co_return drain_server.active_connection_count() == 0;
        };
        return yuan::coroutine::sync_wait(rv, task(rv));
    }();
    drain_server.stop();
    if (drain_thread.joinable()) {
        drain_thread.join();
    }
    if (!require(drain_ok, "Core RPC server should track and close active connections")) {
        return 12;
    }

    return EXIT_SUCCESS;
}
