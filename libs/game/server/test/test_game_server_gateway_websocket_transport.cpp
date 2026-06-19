#include "common/game_rpc_protocol.h"
#include "common/rpc_network.h"
#include "gateway/app/gateway_websocket_transport.h"
#include "yuan/rpc/rpc.h"
#include "yuan/rpc/wire.h"

#include "common/websocket_protocol.h"
#include "common/websocket_utils.h"

#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
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
    constexpr std::uint8_t kClientMaskKey[4] = {0x12, 0x34, 0x56, 0x78};
    constexpr const char *kClientKey = "dGhlIHNhbXBsZSBub25jZQ==";

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

    bool send_all(socket_t s, std::string_view data)
    {
        std::size_t sent = 0;
        while (sent < data.size()) {
#ifdef _WIN32
            const int rc = ::send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
            const ssize_t rc = ::send(s, data.data() + sent, data.size() - sent, 0);
#endif
            if (rc <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(rc);
        }
        return true;
    }

    bool recv_exact(socket_t s, char *dst, std::size_t n)
    {
        std::size_t off = 0;
        while (off < n) {
#ifdef _WIN32
            const int rc = ::recv(s, dst + off, static_cast<int>(n - off), 0);
#else
            const ssize_t rc = ::recv(s, dst + off, n - off, 0);
#endif
            if (rc <= 0) {
                return false;
            }
            off += static_cast<std::size_t>(rc);
        }
        return true;
    }

    bool recv_until_http_headers(socket_t s, std::string &out)
    {
        char buf[2048];
        while (out.find("\r\n\r\n") == std::string::npos && out.size() < 16 * 1024) {
#ifdef _WIN32
            const int rc = ::recv(s, buf, static_cast<int>(sizeof(buf)), 0);
#else
            const ssize_t rc = ::recv(s, buf, sizeof(buf), 0);
#endif
            if (rc <= 0) {
                return false;
            }
            out.append(buf, static_cast<std::size_t>(rc));
        }
        return out.find("\r\n\r\n") != std::string::npos;
    }

    std::string lowercase(std::string text)
    {
        for (char &ch : text) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return text;
    }

    socket_t connect_loopback(std::uint16_t port)
    {
        socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s == kInvalidSocket) {
            return kInvalidSocket;
        }
        set_socket_timeouts(s);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(s, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(s);
            return kInvalidSocket;
        }
        return s;
    }

    std::uint16_t reserve_loopback_port()
    {
        socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener == kInvalidSocket) {
            return 0;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(listener);
            return 0;
        }
        sockaddr_in bound{};
#ifdef _WIN32
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        if (::getsockname(listener, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            close_socket(listener);
            return 0;
        }
        const auto port = ntohs(bound.sin_port);
        close_socket(listener);
        return port;
    }

    std::string websocket_handshake_request(std::uint16_t port)
    {
        return "GET /gateway HTTP/1.1\r\n"
               "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
               "Upgrade: websocket\r\n"
               "Connection: keep-alive, Upgrade\r\n"
               "Sec-WebSocket-Key: " + std::string(kClientKey) + "\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n";
    }

    bool open_websocket(std::uint16_t port, socket_t &client)
    {
        client = connect_loopback(port);
        if (client == kInvalidSocket || !send_all(client, websocket_handshake_request(port))) {
            close_socket(client);
            client = kInvalidSocket;
            return false;
        }
        std::string response;
        if (!recv_until_http_headers(client, response)) {
            close_socket(client);
            client = kInvalidSocket;
            return false;
        }
        const std::string lower = lowercase(response);
        const std::string expected_accept = yuan::net::websocket::WebSocketUtils::generate_server_key(kClientKey);
        return lower.find("101 switching protocols") != std::string::npos &&
               lower.find("upgrade: websocket") != std::string::npos &&
               response.find(expected_accept) != std::string::npos;
    }

    std::string build_ws_frame(std::string_view payload, std::uint8_t opcode, bool masked)
    {
        std::string out;
        out.push_back(static_cast<char>(0x80U | opcode));
        const auto len = payload.size();
        const std::uint8_t mask_bit = masked ? 0x80U : 0U;
        if (len <= yuan::net::websocket::websocket_payload_len_7bit_max) {
            out.push_back(static_cast<char>(mask_bit | static_cast<std::uint8_t>(len)));
        } else if (len <= yuan::net::websocket::websocket_payload_len_16bit_max) {
            out.push_back(static_cast<char>(mask_bit | yuan::net::websocket::websocket_payload_len_16bit_marker));
            out.push_back(static_cast<char>((len >> 8) & 0xffU));
            out.push_back(static_cast<char>(len & 0xffU));
        } else {
            out.push_back(static_cast<char>(mask_bit | yuan::net::websocket::websocket_payload_len_64bit_marker));
            for (int shift = 56; shift >= 0; shift -= 8) {
                out.push_back(static_cast<char>((static_cast<std::uint64_t>(len) >> shift) & 0xffU));
            }
        }
        if (masked) {
            out.append(reinterpret_cast<const char *>(kClientMaskKey), sizeof(kClientMaskKey));
            for (std::size_t i = 0; i < payload.size(); ++i) {
                out.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ kClientMaskKey[i % 4]));
            }
        } else {
            out.append(payload);
        }
        return out;
    }

    struct WsFrame
    {
        std::uint8_t opcode = 0;
        std::string payload;
    };

    std::optional<WsFrame> recv_ws_frame(socket_t s)
    {
        std::uint8_t first_two[2]{};
        if (!recv_exact(s, reinterpret_cast<char *>(first_two), sizeof(first_two))) {
            return std::nullopt;
        }
        std::uint64_t len = first_two[1] & 0x7fU;
        if (len == yuan::net::websocket::websocket_payload_len_16bit_marker) {
            std::uint8_t ext[2]{};
            if (!recv_exact(s, reinterpret_cast<char *>(ext), sizeof(ext))) {
                return std::nullopt;
            }
            len = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
        } else if (len == yuan::net::websocket::websocket_payload_len_64bit_marker) {
            std::uint8_t ext[8]{};
            if (!recv_exact(s, reinterpret_cast<char *>(ext), sizeof(ext))) {
                return std::nullopt;
            }
            len = 0;
            for (std::uint8_t byte : ext) {
                len = (len << 8) | byte;
            }
        }
        if (len > yuan::net::websocket::PACKET_MAX_BYTE) {
            return std::nullopt;
        }
        WsFrame frame;
        frame.opcode = first_two[0] & 0x0fU;
        frame.payload.resize(static_cast<std::size_t>(len));
        if (len > 0 && !recv_exact(s, frame.payload.data(), frame.payload.size())) {
            return std::nullopt;
        }
        return frame;
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
    rpc.register_handler(yuan::rpc::Route{0, 0, "gateway.ws.echo"}, [](const yuan::rpc::Message &message) {
        yuan::rpc::Response response;
        response.request_id = message.request_id;
        response.status = yuan::rpc::RpcStatus::ok;
        response.payload = message.payload;
        response.metadata["connection_id"] = message.metadata.at(yuan::game::server::rpc_network::metadata_key::connection_id);
        return response;
    });

    yuan::net::NetworkRuntime runtime;
    yuan::game::server::GatewayWebSocketTransport transport(rpc, runtime);
    const auto port = reserve_loopback_port();
    if (!require(port != 0, "port should reserve")) {
        return 2;
    }
    if (!require(transport.start(port, 1024 * 1024), "websocket transport should start")) {
        return 3;
    }

    std::jthread server_thread([&] {
        transport.serve();
        (void)runtime.run();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    socket_t client = kInvalidSocket;
    if (!require(open_websocket(port, client), "websocket handshake should succeed")) {
        transport.stop();
        runtime.stop();
        return 4;
    }

    yuan::rpc::Message request;
    request.kind = yuan::rpc::MessageKind::request;
    request.request_id = 42;
    request.route = yuan::rpc::Route{0, 0, "gateway.ws.echo"};
    request.payload = yuan::rpc::Codec<std::string>::encode("hello-ws-gateway");
    yuan::rpc::Bytes rpc_frame;
    if (!require(yuan::rpc::wire::encode_message(request, rpc_frame), "rpc request should encode")) {
        close_socket(client);
        transport.stop();
        runtime.stop();
        return 5;
    }

    const std::string payload(reinterpret_cast<const char *>(rpc_frame.data()), rpc_frame.size());
    if (!require(send_all(client, build_ws_frame(payload, 0x2, true)), "websocket binary frame should send")) {
        close_socket(client);
        transport.stop();
        runtime.stop();
        return 6;
    }

    const auto ws_response = recv_ws_frame(client);
    if (!require(ws_response.has_value(), "websocket response frame should arrive")) {
        close_socket(client);
        transport.stop();
        runtime.stop();
        return 7;
    }
    if (!require(ws_response->opcode == 0x2, "websocket response should be binary")) {
        close_socket(client);
        transport.stop();
        runtime.stop();
        return 8;
    }

    yuan::rpc::wire::FrameStreamDecoder decoder;
    decoder.append(yuan::rpc::Bytes(ws_response->payload.begin(), ws_response->payload.end()));
    auto decoded = decoder.next();
    if (!require(decoded.ok, "rpc response frame should decode")) {
        close_socket(client);
        transport.stop();
        runtime.stop();
        return 9;
    }
    auto response = yuan::rpc::wire::to_response(std::move(decoded.frame));
    if (!require(response.request_id == 42 && response.status == yuan::rpc::RpcStatus::ok,
                 "rpc response should preserve request id/status")) {
        close_socket(client);
        transport.stop();
        runtime.stop();
        return 10;
    }
    if (!require(yuan::rpc::Codec<std::string>::decode(response.payload) == "hello-ws-gateway",
                 "rpc response payload should roundtrip")) {
        close_socket(client);
        transport.stop();
        runtime.stop();
        return 11;
    }
    if (!require(!response.metadata["connection_id"].empty(), "connection id metadata should be attached")) {
        close_socket(client);
        transport.stop();
        runtime.stop();
        return 12;
    }

    close_socket(client);
    transport.stop();
    runtime.stop();

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
