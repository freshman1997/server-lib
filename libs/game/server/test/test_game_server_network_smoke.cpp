#include "game_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
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

    void close_fd(int fd)
    {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    bool read_exact(int fd, yuan::rpc::Bytes &out, std::size_t size)
    {
        out.resize(size);
        std::size_t offset = 0;
        while (offset < size) {
            const auto n = ::recv(fd, out.data() + offset, size - offset, 0);
            if (n <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(n);
        }
        return true;
    }

    bool write_exact(int fd, const yuan::rpc::Bytes &bytes)
    {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto n = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
            if (n <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(n);
        }
        return true;
    }

    std::uint32_t read_body_size(const yuan::rpc::Bytes &header)
    {
        constexpr std::size_t body_size_offset = 12;
        return (static_cast<std::uint32_t>(header[body_size_offset]) << 24) |
               (static_cast<std::uint32_t>(header[body_size_offset + 1]) << 16) |
               (static_cast<std::uint32_t>(header[body_size_offset + 2]) << 8) |
               static_cast<std::uint32_t>(header[body_size_offset + 3]);
    }

    int connect_loopback(std::uint16_t port)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
            close_fd(fd);
            return -1;
        }
        return fd;
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

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::atomic_bool server_ok{true};

    std::thread server_thread([&] {
        int listen_fd = -1;
        int client_fd = -1;
        try {
            listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listen_fd < 0) {
                server_ok = false;
                port_promise.set_value(0);
                return;
            }
            int yes = 1;
            (void)::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(0);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || ::listen(listen_fd, 1) != 0) {
                server_ok = false;
                port_promise.set_value(0);
                close_fd(listen_fd);
                return;
            }

            socklen_t address_len = sizeof(address);
            if (::getsockname(listen_fd, reinterpret_cast<sockaddr *>(&address), &address_len) != 0) {
                server_ok = false;
                port_promise.set_value(0);
                close_fd(listen_fd);
                return;
            }
            port_promise.set_value(ntohs(address.sin_port));

            client_fd = ::accept(listen_fd, nullptr, nullptr);
            close_fd(listen_fd);
            listen_fd = -1;
            if (client_fd < 0) {
                server_ok = false;
                return;
            }

            yuan::rpc::Bytes frame;
            if (!read_exact(client_fd, frame, yuan::rpc::wire::header_size)) {
                server_ok = false;
                close_fd(client_fd);
                return;
            }
            const auto body_size = read_body_size(frame);
            yuan::rpc::Bytes body;
            if (!read_exact(client_fd, body, body_size)) {
                server_ok = false;
                close_fd(client_fd);
                return;
            }
            frame.insert(frame.end(), body.begin(), body.end());

            auto decoded = yuan::rpc::wire::decode_frame(frame);
            if (!decoded.ok) {
                server_ok = false;
                close_fd(client_fd);
                return;
            }
            auto response = tunnel.rpc_server().handle(yuan::rpc::wire::to_message(std::move(decoded.frame)));
            yuan::rpc::Bytes response_frame;
            if (!yuan::rpc::wire::encode_response(response, response_frame) || !write_exact(client_fd, response_frame)) {
                server_ok = false;
            }
            close_fd(client_fd);
        } catch (...) {
            server_ok = false;
            close_fd(client_fd);
            close_fd(listen_fd);
        }
    });

    const auto port = port_future.get();
    if (!require(port != 0, "server should bind a loopback port")) {
        server_thread.join();
        return 2;
    }

    TunnelEnvelope envelope;
    const GameServiceId zone_service{1, 1, GameServiceType::zone, 1, 1};
    envelope.source_service_id = zone_service.pack();
    envelope.target_service_id = global.address().service.pack();
    envelope.source = service_id_key(zone_service);
    envelope.target = service_key(global.address());
    envelope.route.name = std::string(route::global_echo);
    envelope.payload = yuan::rpc::Codec<std::string>::encode("hello-tcp-global");
    envelope.metadata["trace_id"] = "network-smoke";

    yuan::rpc::Bytes envelope_payload;
    if (!require(encode_tunnel_envelope(envelope, envelope_payload), "tunnel envelope should encode")) {
        server_thread.join();
        return 3;
    }

    yuan::rpc::Message message;
    message.kind = yuan::rpc::MessageKind::request;
    message.request_id = 42;
    message.route.name = std::string(route::tunnel_forward);
    message.payload = std::move(envelope_payload);

    yuan::rpc::Bytes request_frame;
    if (!require(yuan::rpc::wire::encode_message(message, request_frame), "request frame should encode")) {
        server_thread.join();
        return 4;
    }

    const int fd = connect_loopback(port);
    if (!require(fd >= 0, "client should connect to loopback server")) {
        server_thread.join();
        return 5;
    }
    if (!require(write_exact(fd, request_frame), "client should send request frame")) {
        close_fd(fd);
        server_thread.join();
        return 6;
    }

    yuan::rpc::Bytes response_frame;
    if (!require(read_exact(fd, response_frame, yuan::rpc::wire::header_size), "client should read response header")) {
        close_fd(fd);
        server_thread.join();
        return 7;
    }
    const auto response_body_size = read_body_size(response_frame);
    yuan::rpc::Bytes response_body;
    if (!require(read_exact(fd, response_body, response_body_size), "client should read response body")) {
        close_fd(fd);
        server_thread.join();
        return 8;
    }
    close_fd(fd);
    response_frame.insert(response_frame.end(), response_body.begin(), response_body.end());

    auto decoded_response = yuan::rpc::wire::decode_frame(response_frame);
    if (!require(decoded_response.ok, "response frame should decode")) {
        server_thread.join();
        return 9;
    }
    const auto response = yuan::rpc::wire::to_response(std::move(decoded_response.frame));
    if (!require(response.status == yuan::rpc::RpcStatus::ok, "tcp tunnel forward should succeed")) {
        server_thread.join();
        return 10;
    }
    if (!require(yuan::rpc::Codec<std::string>::decode(response.payload) == "hello-tcp-global", "global tcp payload mismatch")) {
        server_thread.join();
        return 11;
    }
    if (!require(response.metadata.find("global.node") != response.metadata.end(), "global metadata should return over tcp")) {
        server_thread.join();
        return 12;
    }

    server_thread.join();
    if (!require(server_ok.load(), "tcp server thread should complete successfully")) {
        return 13;
    }
    if (!require(global.request_count() == 1, "global should receive exactly one tcp request")) {
        return 14;
    }
    return EXIT_SUCCESS;
}
