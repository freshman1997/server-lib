#include "common/rpc_network.h"
#include "common/login_token.h"
#include "gateway/app/gateway_kcp_transport.h"
#include "yuan/rpc/rpc.h"
#include "yuan/rpc/wire.h"

#include "base/time.h"

#include "ikcp.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
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
    constexpr std::uint64_t kSecret = 987654321;
    constexpr yuan::game::server::PackedGameServiceId kZoneServiceId = 4504699675869185ULL;

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

    std::optional<yuan::rpc::Bytes> recv_kcp_payload(socket_t socket, ikcpcb *kcp)
    {
        std::vector<char> recv_buffer(64 * 1024);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto packet = recv_datagram(socket);
            if (!packet.has_value()) {
                break;
            }
            if (packet->empty() || (*packet)[0] != yuan::game::server::GatewayKcpTransport::kKcpPacket) {
                continue;
            }
            if (ikcp_input(kcp, reinterpret_cast<const char *>(packet->data() + 1), static_cast<long>(packet->size() - 1)) < 0) {
                break;
            }
            for (;;) {
                const int received = ikcp_recv(kcp, recv_buffer.data(), static_cast<int>(recv_buffer.size()));
                if (received <= 0) {
                    break;
                }
                return yuan::rpc::Bytes(recv_buffer.begin(), recv_buffer.begin() + received);
            }
        }
        return std::nullopt;
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
        packet.push_back(yuan::game::server::GatewayKcpTransport::kKcpPacket);
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

    yuan::rpc::Server rpc;
    rpc.register_handler(yuan::rpc::Route{0, 0, "gateway.kcp.echo"}, [](const yuan::rpc::Message &message) {
        yuan::rpc::Response response;
        response.request_id = message.request_id;
        response.status = yuan::rpc::RpcStatus::ok;
        response.payload = message.payload;
        response.metadata["connection_id"] = message.metadata.at(yuan::game::server::rpc_network::metadata_key::connection_id);
        return response;
    });

    yuan::net::NetworkRuntime runtime;
    yuan::game::server::GatewayKcpTransport transport(rpc, runtime);
    transport.set_handshake_validator([](const yuan::net::InetAddress &, const std::vector<std::uint8_t> &payload) {
        const std::string text(payload.begin(), payload.end());
        const auto separator = text.find('|');
        const auto token = separator == std::string::npos ? text : text.substr(0, separator);
        if (!yuan::game::server::decode_login_token_id(token, yuan::base::time::steady_now_ms(), kSecret)) {
            return yuan::net::KcpServerSession::HandshakeDecision{};
        }
        std::uint32_t migrate_conv = 0;
        if (separator != std::string::npos) {
            migrate_conv = static_cast<std::uint32_t>(std::stoul(text.substr(separator + 1)));
        }
        return yuan::net::KcpServerSession::HandshakeDecision{true, migrate_conv};
    });
    const auto port = reserve_udp_port();
    if (!require(port != 0, "udp port should reserve")) {
        return 2;
    }
    yuan::net::KcpServerSession::Config config;
    config.host = "127.0.0.1";
    config.port = port;
    config.allow_migration = true;
    if (!require(transport.start(config, 1024 * 1024), "kcp transport should start")) {
        return 3;
    }
    std::jthread server_thread([&] { (void)runtime.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    sockaddr_in server_addr{};
    socket_t client_socket = open_udp_client(port, server_addr);
    if (!require(client_socket != kInvalidSocket, "udp client should open")) {
        transport.stop();
        runtime.stop();
        return 4;
    }

    std::vector<std::uint8_t> handshake;
    handshake.push_back(yuan::game::server::GatewayKcpTransport::kHandshakePacket);
    const auto login_token = yuan::game::server::encode_login_token_id(kZoneServiceId, yuan::base::time::steady_now_ms() + 10000, kSecret);
    handshake.insert(handshake.end(), login_token.begin(), login_token.end());
    if (!require(send_datagram(client_socket, server_addr, handshake), "handshake should send")) {
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 5;
    }
    const auto ack = recv_datagram(client_socket);
    if (!require(ack.has_value() && ack->size() == 5 && (*ack)[0] == yuan::game::server::GatewayKcpTransport::kHandshakeAckPacket,
                 "handshake ack should arrive")) {
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 6;
    }
    const auto conv = read_u32_be(ack->data() + 1);
    if (!require(conv != 0, "handshake conv should be non-zero")) {
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 7;
    }

    KcpClient kcp_client{client_socket, &server_addr};
    ikcpcb *kcp = ikcp_create(conv, &kcp_client);
    kcp->output = &kcp_output;
    ikcp_wndsize(kcp, 128, 128);
    ikcp_nodelay(kcp, 1, 10, 1, 1);

    yuan::rpc::Message request;
    request.kind = yuan::rpc::MessageKind::request;
    request.request_id = 77;
    request.route = yuan::rpc::Route{0, 0, "gateway.kcp.echo"};
    request.payload = yuan::rpc::Codec<std::string>::encode("hello-kcp-gateway");
    yuan::rpc::Bytes frame;
    if (!require(yuan::rpc::wire::encode_message(request, frame), "rpc request should encode")) {
        ikcp_release(kcp);
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 8;
    }
    if (!require(ikcp_send(kcp, reinterpret_cast<const char *>(frame.data()), static_cast<int>(frame.size())) >= 0,
                 "kcp request should queue")) {
        ikcp_release(kcp);
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 9;
    }
    const auto now_ms = static_cast<IUINT32>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    ikcp_update(kcp, now_ms);
    ikcp_flush(kcp);

    auto response_frame = recv_kcp_payload(client_socket, kcp);
    if (!require(response_frame.has_value() && !response_frame->empty(), "kcp rpc response should arrive")) {
        ikcp_release(kcp);
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 10;
    }

    yuan::rpc::wire::FrameStreamDecoder decoder;
    decoder.append(std::move(*response_frame));
    auto decoded = decoder.next();
    if (!require(decoded.ok, "rpc response should decode")) {
        ikcp_release(kcp);
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 11;
    }
    auto response = yuan::rpc::wire::to_response(std::move(decoded.frame));
    if (!require(response.request_id == 77 && response.status == yuan::rpc::RpcStatus::ok,
                 "rpc response should preserve request id/status")) {
        ikcp_release(kcp);
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 12;
    }
    if (!require(yuan::rpc::Codec<std::string>::decode(response.payload) == "hello-kcp-gateway",
                 "rpc response payload should roundtrip")) {
        ikcp_release(kcp);
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 13;
    }
    if (!require(!response.metadata["connection_id"].empty(), "connection id metadata should be attached")) {
        ikcp_release(kcp);
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 14;
    }

    sockaddr_in migrated_server_addr{};
    socket_t migrated_socket = open_udp_client(port, migrated_server_addr);
    if (!require(migrated_socket != kInvalidSocket, "migrated udp client should open")) {
        ikcp_release(kcp);
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 15;
    }
    KcpClient migrated_kcp_client{migrated_socket, &migrated_server_addr};
    kcp->user = &migrated_kcp_client;
    const auto migrate_payload = login_token + "|" + std::to_string(conv);
    std::vector<std::uint8_t> migrate_handshake;
    migrate_handshake.push_back(yuan::game::server::GatewayKcpTransport::kHandshakePacket);
    migrate_handshake.insert(migrate_handshake.end(), migrate_payload.begin(), migrate_payload.end());
    if (!require(send_datagram(migrated_socket, migrated_server_addr, migrate_handshake), "migration handshake should send")) {
        close_socket(migrated_socket);
        ikcp_release(kcp);
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 16;
    }
    const auto migrate_ack = recv_datagram(migrated_socket);
    if (!require(migrate_ack.has_value() && migrate_ack->size() == 5 && (*migrate_ack)[0] == yuan::game::server::GatewayKcpTransport::kHandshakeAckPacket && read_u32_be(migrate_ack->data() + 1) == conv,
                 "migration ack should return existing conv")) {
        close_socket(migrated_socket);
        ikcp_release(kcp);
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 17;
    }
    request.request_id = 78;
    request.payload = yuan::rpc::Codec<std::string>::encode("hello-kcp-migration");
    frame.clear();
    if (!require(yuan::rpc::wire::encode_message(request, frame), "migration rpc request should encode")) {
        close_socket(migrated_socket);
        ikcp_release(kcp);
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 18;
    }
    if (!require(ikcp_send(kcp, reinterpret_cast<const char *>(frame.data()), static_cast<int>(frame.size())) >= 0,
                 "migrated kcp request should queue")) {
        close_socket(migrated_socket);
        ikcp_release(kcp);
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 19;
    }
    ikcp_update(kcp, static_cast<IUINT32>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
    ikcp_flush(kcp);
    auto migrated_response_frame = recv_kcp_payload(migrated_socket, kcp);
    if (!require(migrated_response_frame.has_value() && !migrated_response_frame->empty(), "migrated kcp rpc response should arrive")) {
        close_socket(migrated_socket);
        ikcp_release(kcp);
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 20;
    }
    yuan::rpc::wire::FrameStreamDecoder migrated_decoder;
    migrated_decoder.append(std::move(*migrated_response_frame));
    auto migrated_decoded = migrated_decoder.next();
    if (!require(migrated_decoded.ok, "migrated rpc response should decode")) {
        close_socket(migrated_socket);
        ikcp_release(kcp);
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 21;
    }
    auto migrated_response = yuan::rpc::wire::to_response(std::move(migrated_decoded.frame));
    if (!require(migrated_response.request_id == 78 && yuan::rpc::Codec<std::string>::decode(migrated_response.payload) == "hello-kcp-migration",
                 "migrated rpc response should roundtrip")) {
        close_socket(migrated_socket);
        ikcp_release(kcp);
        close_socket(client_socket);
        transport.stop();
        runtime.stop();
        return 22;
    }

    close_socket(migrated_socket);
    ikcp_release(kcp);
    close_socket(client_socket);
    transport.stop();
    runtime.stop();

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
