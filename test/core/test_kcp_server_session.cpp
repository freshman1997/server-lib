#include "ikcp.h"
#include "net/runtime/network_runtime.h"
#include "net/session/kcp_server_session.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace
{
    constexpr int kSocketTimeoutMs = 2500;
    constexpr std::uint8_t kHandshakeMagic[] = {'Y', 'K', 'C', 'P', '1'};

    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

    void close_socket(socket_t s)
    {
        if (s == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        ::closesocket(s);
#else
        ::close(s);
#endif
    }

    void set_socket_timeouts(socket_t s)
    {
#ifdef _WIN32
        const DWORD timeout = static_cast<DWORD>(kSocketTimeoutMs);
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        (void)::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
        timeval tv{};
        tv.tv_sec = kSocketTimeoutMs / 1000;
        tv.tv_usec = (kSocketTimeoutMs % 1000) * 1000;
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    }

    std::uint16_t reserve_udp_port()
    {
        socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (s == kInvalidSocket) {
            return 0;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(s, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(s);
            return 0;
        }
        sockaddr_in bound{};
#ifdef _WIN32
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        if (::getsockname(s, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            close_socket(s);
            return 0;
        }
        const auto port = ntohs(bound.sin_port);
        close_socket(s);
        return port;
    }

    socket_t open_udp_client(std::uint16_t server_port, sockaddr_in &server_addr)
    {
        socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (s == kInvalidSocket) {
            return kInvalidSocket;
        }
        set_socket_timeouts(s);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        if (::bind(s, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) != 0) {
            close_socket(s);
            return kInvalidSocket;
        }
        server_addr = {};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        server_addr.sin_port = htons(server_port);
        return s;
    }

    bool send_datagram(socket_t s, const sockaddr_in &addr, const std::vector<std::uint8_t> &bytes)
    {
#ifdef _WIN32
        const int rc = ::sendto(s, reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()), 0, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
#else
        const ssize_t rc = ::sendto(s, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
#endif
        return rc == static_cast<decltype(rc)>(bytes.size());
    }

    std::optional<std::vector<std::uint8_t>> recv_datagram(socket_t s)
    {
        std::vector<std::uint8_t> bytes(2048);
#ifdef _WIN32
        const int rc = ::recv(s, reinterpret_cast<char *>(bytes.data()), static_cast<int>(bytes.size()), 0);
#else
        const ssize_t rc = ::recv(s, bytes.data(), bytes.size(), 0);
#endif
        if (rc <= 0) {
            return std::nullopt;
        }
        bytes.resize(static_cast<std::size_t>(rc));
        return bytes;
    }

    std::uint32_t read_u32_be(const std::uint8_t *data)
    {
        return (static_cast<std::uint32_t>(data[0]) << 24) |
               (static_cast<std::uint32_t>(data[1]) << 16) |
               (static_cast<std::uint32_t>(data[2]) << 8) |
               static_cast<std::uint32_t>(data[3]);
    }

    struct KcpClient
    {
        socket_t socket = kInvalidSocket;
        const sockaddr_in *server_addr = nullptr;
    };

    int kcp_output(const char *buf, int len, ikcpcb *, void *user)
    {
        auto *client = static_cast<KcpClient *>(user);
        if (!client || !client->server_addr || client->socket == kInvalidSocket || len <= 0) {
            return -1;
        }
        std::vector<std::uint8_t> packet;
        packet.reserve(static_cast<std::size_t>(len) + 1);
        packet.push_back(yuan::net::KcpServerSession::Config{}.kcp_packet_type);
        packet.insert(packet.end(), reinterpret_cast<const std::uint8_t *>(buf), reinterpret_cast<const std::uint8_t *>(buf) + len);
        return send_datagram(client->socket, *client->server_addr, packet) ? len : -1;
    }
}

int main()
{
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    yuan::net::NetworkRuntime runtime;
    yuan::net::KcpServerSession server(runtime);
    std::atomic<std::uint64_t> connection_id{0};
    std::atomic_bool received_payload{false};
    std::atomic<int> payload_count{0};
    server.set_data_callback([&](std::uint64_t id, std::vector<std::uint8_t> payload) {
        connection_id.store(id, std::memory_order_relaxed);
        if (payload == std::vector<std::uint8_t>{'p', 'i', 'n', 'g'}) {
            received_payload.store(true, std::memory_order_relaxed);
            payload_count.fetch_add(1, std::memory_order_relaxed);
        }
        (void)server.send(id, std::vector<std::uint8_t>{'p', 'o', 'n', 'g'});
    });

    const auto port = reserve_udp_port();
    if (!require(port != 0, "udp port should reserve")) {
        return 2;
    }
    yuan::net::KcpServerSession::Config config;
    config.host = "127.0.0.1";
    config.port = port;
    config.first_connection_id = 7000;
    config.allow_migration = true;
    config.max_sessions_per_ip = 1;
    server.set_handshake_validator([](const yuan::net::InetAddress &, const std::vector<std::uint8_t> &payload) {
        if (payload == std::vector<std::uint8_t>(std::begin(kHandshakeMagic), std::end(kHandshakeMagic))) {
            return yuan::net::KcpServerSession::HandshakeDecision{true, 0};
        }
        constexpr std::string_view prefix = "MIG:";
        const std::string text(payload.begin(), payload.end());
        if (!text.starts_with(prefix)) {
            return yuan::net::KcpServerSession::HandshakeDecision{};
        }
        return yuan::net::KcpServerSession::HandshakeDecision{true, static_cast<std::uint32_t>(std::stoul(text.substr(prefix.size())))};
    });
    if (!require(server.start(std::move(config)), "kcp server session should start")) {
        return 3;
    }
    std::jthread server_thread([&] { (void)runtime.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    sockaddr_in server_addr{};
    socket_t client_socket = open_udp_client(port, server_addr);
    if (!require(client_socket != kInvalidSocket, "udp client should open")) {
        server.stop();
        runtime.stop();
        return 4;
    }

    std::vector<std::uint8_t> handshake;
    handshake.push_back(yuan::net::KcpServerSession::Config{}.handshake_packet_type);
    handshake.insert(handshake.end(), std::begin(kHandshakeMagic), std::end(kHandshakeMagic));
    if (!require(send_datagram(client_socket, server_addr, handshake), "handshake should send")) {
        close_socket(client_socket);
        server.stop();
        runtime.stop();
        return 5;
    }
    const auto ack = recv_datagram(client_socket);
    if (!require(ack.has_value() && ack->size() == 5 && (*ack)[0] == yuan::net::KcpServerSession::Config{}.handshake_ack_packet_type,
                 "handshake ack should arrive")) {
        close_socket(client_socket);
        server.stop();
        runtime.stop();
        return 6;
    }
    const auto conv = read_u32_be(ack->data() + 1);
    if (!require(conv != 0, "handshake conv should be non-zero")) {
        close_socket(client_socket);
        server.stop();
        runtime.stop();
        return 7;
    }

    KcpClient client{client_socket, &server_addr};
    ikcpcb *kcp = ikcp_create(conv, &client);
    kcp->output = &kcp_output;
    ikcp_wndsize(kcp, 128, 128);
    ikcp_nodelay(kcp, 1, 10, 1, 1);
    const std::vector<std::uint8_t> ping{'p', 'i', 'n', 'g'};
    if (!require(ikcp_send(kcp, reinterpret_cast<const char *>(ping.data()), static_cast<int>(ping.size())) >= 0,
                 "kcp ping should queue")) {
        ikcp_release(kcp);
        close_socket(client_socket);
        server.stop();
        runtime.stop();
        return 8;
    }
    const auto now_ms = static_cast<IUINT32>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    ikcp_update(kcp, now_ms);
    ikcp_flush(kcp);

    std::vector<char> recv_buffer(4096);
    std::vector<std::uint8_t> pong;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && pong.empty()) {
        const auto packet = recv_datagram(client_socket);
        if (!packet.has_value()) {
            break;
        }
        if (packet->empty() || (*packet)[0] != yuan::net::KcpServerSession::Config{}.kcp_packet_type) {
            continue;
        }
        if (ikcp_input(kcp, reinterpret_cast<const char *>(packet->data() + 1), static_cast<long>(packet->size() - 1)) < 0) {
            break;
        }
        const int received = ikcp_recv(kcp, recv_buffer.data(), static_cast<int>(recv_buffer.size()));
        if (received > 0) {
            pong.assign(recv_buffer.begin(), recv_buffer.begin() + received);
        }
    }

    if (!require(received_payload.load(std::memory_order_relaxed), "server should receive client payload")) {
        ikcp_release(kcp);
        close_socket(client_socket);
        server.stop();
        runtime.stop();
        return 9;
    }
    if (!require(connection_id.load(std::memory_order_relaxed) == 7000, "connection id should use configured base")) {
        ikcp_release(kcp);
        close_socket(client_socket);
        server.stop();
        runtime.stop();
        return 10;
    }

    sockaddr_in second_server_addr{};
    socket_t second_socket = open_udp_client(port, second_server_addr);
    if (!require(second_socket != kInvalidSocket, "second udp client should open")) {
        ikcp_release(kcp);
        close_socket(client_socket);
        server.stop();
        runtime.stop();
        return 12;
    }
    if (!require(send_datagram(second_socket, second_server_addr, handshake), "second handshake should send")) {
        close_socket(second_socket);
        ikcp_release(kcp);
        close_socket(client_socket);
        server.stop();
        runtime.stop();
        return 13;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!require(server.metrics().handshakes_rejected >= 1, "second same-ip session should be rejected")) {
        close_socket(second_socket);
        ikcp_release(kcp);
        close_socket(client_socket);
        server.stop();
        runtime.stop();
        return 14;
    }
    if (!require(pong == std::vector<std::uint8_t>{'p', 'o', 'n', 'g'}, "client should receive server payload")) {
        close_socket(second_socket);
        ikcp_release(kcp);
        close_socket(client_socket);
        server.stop();
        runtime.stop();
        return 11;
    }

    KcpClient migrated_client{second_socket, &second_server_addr};
    kcp->user = &migrated_client;
    const std::string migrate_text = "MIG:" + std::to_string(conv);
    std::vector<std::uint8_t> migrate_handshake;
    migrate_handshake.push_back(yuan::net::KcpServerSession::Config{}.handshake_packet_type);
    migrate_handshake.insert(migrate_handshake.end(), migrate_text.begin(), migrate_text.end());
    if (!require(send_datagram(second_socket, second_server_addr, migrate_handshake), "migration handshake should send")) {
        close_socket(second_socket);
        ikcp_release(kcp);
        close_socket(client_socket);
        server.stop();
        runtime.stop();
        return 15;
    }
    const auto migrate_ack = recv_datagram(second_socket);
    if (!require(migrate_ack.has_value() && migrate_ack->size() == 5 && (*migrate_ack)[0] == yuan::net::KcpServerSession::Config{}.handshake_ack_packet_type && read_u32_be(migrate_ack->data() + 1) == conv,
                 "migration ack should return existing conv")) {
        close_socket(second_socket);
        ikcp_release(kcp);
        close_socket(client_socket);
        server.stop();
        runtime.stop();
        return 16;
    }
    if (!require(ikcp_send(kcp, reinterpret_cast<const char *>(ping.data()), static_cast<int>(ping.size())) >= 0,
                 "migrated kcp ping should queue")) {
        close_socket(second_socket);
        ikcp_release(kcp);
        close_socket(client_socket);
        server.stop();
        runtime.stop();
        return 17;
    }
    ikcp_update(kcp, static_cast<IUINT32>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
    ikcp_flush(kcp);
    pong.clear();
    const auto migration_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < migration_deadline && pong.empty()) {
        const auto packet = recv_datagram(second_socket);
        if (!packet.has_value()) {
            break;
        }
        if (packet->empty() || (*packet)[0] != yuan::net::KcpServerSession::Config{}.kcp_packet_type) {
            continue;
        }
        if (ikcp_input(kcp, reinterpret_cast<const char *>(packet->data() + 1), static_cast<long>(packet->size() - 1)) < 0) {
            break;
        }
        const int received = ikcp_recv(kcp, recv_buffer.data(), static_cast<int>(recv_buffer.size()));
        if (received > 0) {
            pong.assign(recv_buffer.begin(), recv_buffer.begin() + received);
        }
    }
    if (!require(payload_count.load(std::memory_order_relaxed) >= 2 && pong == std::vector<std::uint8_t>{'p', 'o', 'n', 'g'},
                 "migrated client should continue same kcp session")) {
        close_socket(second_socket);
        ikcp_release(kcp);
        close_socket(client_socket);
        server.stop();
        runtime.stop();
        return 18;
    }

    close_socket(second_socket);

    ikcp_release(kcp);
    close_socket(client_socket);
    server.stop();
    runtime.stop();

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
