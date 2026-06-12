#include "http/http_service.h"
#include "proxy_api.h"
#include "reverse_proxy.h"
#include "proxy/websocket_proxy.h"
#include "common/websocket_protocol.h"
#include "common/websocket_utils.h"

#include <atomic>
#include <chrono>
#include <cctype>
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
    constexpr int SOCKET_TIMEOUT_MS = 2500;
    constexpr int LISTENER_BACKLOG = 16;
    constexpr std::size_t MAX_HTTP_HEADER_BYTES = 16 * 1024;
    constexpr std::size_t MAX_FRAME_PAYLOAD_BYTES = 1024 * 1024;
    constexpr uint8_t CLIENT_MASK_KEY[4] = {0x12, 0x34, 0x56, 0x78};
    constexpr uint8_t SERVER_MASK_KEY[4] = {0x87, 0x65, 0x43, 0x21};
    constexpr const char *CLIENT_KEY = "dGhlIHNhbXBsZSBub25jZQ==";
    constexpr const char *WS_PATH = "/ws/echo";

    int g_failed = 0;

    void check(bool cond, const char *msg)
    {
        if (!cond) {
            ++g_failed;
            std::cerr << "[FAIL] " << msg << '\n';
        }
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

    void set_socket_timeouts(socket_t s, int timeout_ms = SOCKET_TIMEOUT_MS)
    {
#ifdef _WIN32
        const DWORD timeout = static_cast<DWORD>(timeout_ms);
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        (void)::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
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

    bool recv_until_http_headers(socket_t s, std::string &out, std::size_t max_bytes = MAX_HTTP_HEADER_BYTES)
    {
        char buf[2048];
        while (out.find("\r\n\r\n") == std::string::npos && out.size() < max_bytes) {
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

    socket_t connect_loopback(uint16_t port)
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

    uint16_t reserve_tcp_port()
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
        const uint16_t port = ntohs(bound.sin_port);
        close_socket(listener);
        return port;
    }

    std::string build_frame(std::string_view payload, uint8_t opcode, bool masked, const uint8_t mask_key[4])
    {
        std::string out;
        out.push_back(static_cast<char>(0x80U | opcode));
        const std::size_t len = payload.size();
        const uint8_t mask_bit = masked ? 0x80U : 0U;
        if (len <= 125) {
            out.push_back(static_cast<char>(mask_bit | static_cast<uint8_t>(len)));
        } else if (len <= 0xffff) {
            out.push_back(static_cast<char>(mask_bit | 126U));
            out.push_back(static_cast<char>((len >> 8) & 0xffU));
            out.push_back(static_cast<char>(len & 0xffU));
        } else {
            out.push_back(static_cast<char>(mask_bit | 127U));
            for (int shift = 56; shift >= 0; shift -= 8) {
                out.push_back(static_cast<char>((static_cast<uint64_t>(len) >> shift) & 0xffU));
            }
        }

        if (masked) {
            out.append(reinterpret_cast<const char *>(mask_key), 4);
            for (std::size_t i = 0; i < payload.size(); ++i) {
                out.push_back(static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask_key[i % 4]));
            }
        } else {
            out.append(payload);
        }
        return out;
    }

    struct WsFrame
    {
        uint8_t opcode = 0;
        bool masked = false;
        std::string payload;
    };

    std::optional<WsFrame> recv_frame(socket_t s)
    {
        uint8_t first_two[2]{};
        if (!recv_exact(s, reinterpret_cast<char *>(first_two), sizeof(first_two))) {
            return std::nullopt;
        }
        if ((first_two[0] & 0x80U) == 0) {
            return std::nullopt;
        }

        uint64_t len = first_two[1] & 0x7fU;
        if (len == 126) {
            uint8_t ext[2]{};
            if (!recv_exact(s, reinterpret_cast<char *>(ext), sizeof(ext))) {
                return std::nullopt;
            }
            len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            uint8_t ext[8]{};
            if (!recv_exact(s, reinterpret_cast<char *>(ext), sizeof(ext))) {
                return std::nullopt;
            }
            len = 0;
            for (uint8_t byte : ext) {
                len = (len << 8) | byte;
            }
        }
        if (len > MAX_FRAME_PAYLOAD_BYTES) {
            return std::nullopt;
        }

        WsFrame frame;
        frame.opcode = first_two[0] & 0x0fU;
        frame.masked = (first_two[1] & 0x80U) != 0;

        uint8_t mask_key[4]{};
        if (frame.masked && !recv_exact(s, reinterpret_cast<char *>(mask_key), sizeof(mask_key))) {
            return std::nullopt;
        }

        frame.payload.resize(static_cast<std::size_t>(len));
        if (len > 0 && !recv_exact(s, frame.payload.data(), frame.payload.size())) {
            return std::nullopt;
        }
        if (frame.masked) {
            for (std::size_t i = 0; i < frame.payload.size(); ++i) {
                frame.payload[i] = static_cast<char>(static_cast<uint8_t>(frame.payload[i]) ^ mask_key[i % 4]);
            }
        }
        return frame;
    }

    bool start_ws_backend(uint16_t port,
                          std::atomic_bool &ready,
                          std::atomic_bool &stop,
                          std::atomic_bool &saw_masked_backend_frame,
                          std::atomic_bool &saw_forwarded_headers)
    {
        socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener == kInvalidSocket) {
            return false;
        }
        int reuse = 1;
#ifdef _WIN32
        (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#else
        (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0 ||
            ::listen(listener, LISTENER_BACKLOG) != 0) {
            close_socket(listener);
            return false;
        }
        set_socket_timeouts(listener, 500);
        ready.store(true);

        while (!stop.load()) {
            socket_t client = ::accept(listener, nullptr, nullptr);
            if (client == kInvalidSocket) {
                continue;
            }
            set_socket_timeouts(client);

            std::string request;
            if (!recv_until_http_headers(client, request)) {
                close_socket(client);
                continue;
            }

            const std::string lower = lowercase(request);
            const bool is_upgrade = lower.find("get " + std::string(WS_PATH) + " http/1.1") != std::string::npos &&
                                    lower.find("upgrade: websocket") != std::string::npos &&
                                    lower.find("sec-websocket-key:") != std::string::npos;
            if (!is_upgrade) {
                close_socket(client);
                continue;
            }
            saw_forwarded_headers.store(lower.find("origin: https://client.example") != std::string::npos &&
                                        lower.find("x-forwarded-for:") != std::string::npos &&
                                        lower.find("x-real-ip:") != std::string::npos &&
                                        lower.find("x-forwarded-proto: http") != std::string::npos);

            const std::string key_marker = "sec-websocket-key:";
            const auto key_pos = lower.find(key_marker);
            std::string key;
            if (key_pos != std::string::npos) {
                std::size_t begin = key_pos + key_marker.size();
                while (begin < request.size() && std::isspace(static_cast<unsigned char>(request[begin]))) {
                    ++begin;
                }
                std::size_t end = request.find("\r\n", begin);
                key = request.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            }
            const std::string accept = yuan::net::websocket::WebSocketUtils::generate_server_key(key);
            const std::string response =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: keep-alive, Upgrade\r\n"
                "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
            if (!send_all(client, response)) {
                close_socket(client);
                continue;
            }

            auto frame = recv_frame(client);
            if (frame && frame->masked && frame->opcode == static_cast<uint8_t>(yuan::net::websocket::OpCodeType::type_text_frame)) {
                saw_masked_backend_frame.store(true);
                const std::string echo = "echo:" + frame->payload;
                (void)send_all(client, build_frame(echo,
                                                   static_cast<uint8_t>(yuan::net::websocket::OpCodeType::type_text_frame),
                                                   false,
                                                   SERVER_MASK_KEY));
            }
            close_socket(client);
        }

        close_socket(listener);
        return true;
    }

    std::string websocket_handshake_request(uint16_t port, std::string_view key = CLIENT_KEY)
    {
        return "GET " + std::string(WS_PATH) + " HTTP/1.1\r\n"
               "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
               "upgrade: websocket\r\n"
               "connection: keep-alive, Upgrade\r\n"
               "sec-websocket-key: " + std::string(key) + "\r\n"
               "sec-websocket-version: 13\r\n"
               "origin: https://client.example\r\n\r\n";
    }

    void test_websocket_proxy_echo(uint16_t proxy_port,
                                   std::atomic_bool &saw_masked_backend_frame,
                                   std::atomic_bool &saw_forwarded_headers)
    {
        socket_t client = connect_loopback(proxy_port);
        check(client != kInvalidSocket, "masked client should connect to websocket proxy");
        if (client == kInvalidSocket) {
            return;
        }

        check(send_all(client, websocket_handshake_request(proxy_port)), "masked client should send websocket upgrade");
        std::string response;
        check(recv_until_http_headers(client, response), "masked client should receive 101 response");
        const std::string lower = lowercase(response);
        check(lower.find("101 switching protocols") != std::string::npos, "proxy should return 101 switching protocols");
        check(lower.find("connection: upgrade") != std::string::npos, "proxy response should include Connection: Upgrade");
        check(lower.find("upgrade: websocket") != std::string::npos, "proxy response should include Upgrade: websocket");
        const std::string expected_accept = yuan::net::websocket::WebSocketUtils::generate_server_key(CLIENT_KEY);
        check(response.find(expected_accept) != std::string::npos, "proxy should answer with accept for browser client key");

        const std::string payload = "browser-masked-message";
        const std::string frame = build_frame(payload,
                                              static_cast<uint8_t>(yuan::net::websocket::OpCodeType::type_text_frame),
                                              true,
                                              CLIENT_MASK_KEY);
        check(send_all(client, frame), "masked client should send masked text frame");

        auto reply = recv_frame(client);
        check(reply.has_value(), "masked client should receive proxied websocket reply");
        if (reply) {
            check(!reply->masked, "server-to-client proxy frame should be unmasked");
            check(reply->opcode == static_cast<uint8_t>(yuan::net::websocket::OpCodeType::type_text_frame),
                  "server-to-client proxy frame should be text");
            check(reply->payload == "echo:" + payload, "proxied websocket payload should round-trip");
        }
        check(saw_masked_backend_frame.load(), "backend should receive masked proxy client frame");
        check(saw_forwarded_headers.load(), "backend handshake should receive forwarded websocket headers");
        close_socket(client);
    }

    void test_invalid_websocket_key_rejected(uint16_t proxy_port)
    {
        socket_t client = connect_loopback(proxy_port);
        check(client != kInvalidSocket, "invalid-key client should connect to websocket proxy");
        if (client == kInvalidSocket) {
            return;
        }
        check(send_all(client, websocket_handshake_request(proxy_port, "invalid-key")),
              "invalid-key client should send upgrade request");

        std::string response;
        const bool has_headers = recv_until_http_headers(client, response);
        check(has_headers, "invalid websocket key should receive HTTP error response");
        check(response.find("101 Switching Protocols") == std::string::npos,
              "invalid websocket key must not be upgraded");
        close_socket(client);
    }
}

int main()
{
#ifdef _WIN32
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    const uint16_t backend_port = reserve_tcp_port();
    const uint16_t proxy_port = reserve_tcp_port();
    check(backend_port != 0, "should reserve websocket backend port");
    check(proxy_port != 0, "should reserve websocket proxy port");
    if (backend_port == 0 || proxy_port == 0) {
        return 1;
    }

    std::atomic_bool backend_ready{false};
    std::atomic_bool backend_stop{false};
    std::atomic_bool saw_masked_backend_frame{false};
    std::atomic_bool saw_forwarded_headers{false};
    std::thread backend_thread([&]() {
        (void)start_ws_backend(backend_port,
                               backend_ready,
                               backend_stop,
                               saw_masked_backend_frame,
                               saw_forwarded_headers);
    });
    for (int i = 0; i < 100 && !backend_ready.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(backend_ready.load(), "websocket backend should start");

    yuan::net::http::HttpServerConfig config;
    config.enable_keep_alive = false;
    config.write_timeout_ms = 1000;
    yuan::server::HttpService service(proxy_port, config);
    service.set_admin_dashboard_enabled(false);
    service.set_server_configurator([backend_port](yuan::server::HttpService &svc) {
        auto *proxy = svc.server().ensure_proxy();
        if (!proxy) {
            return false;
        }

        yuan::net::http::ProxyRoute route;
        route.match_pattern = "/ws/";
        route.strip_prefix = false;
        route.max_retries = 0;
        route.connect_timeout_ms = 500;
        route.read_timeout_ms = 1000;
        route.write_timeout_ms = 1000;
        route.targets.push_back(yuan::net::http::ProxyTarget{"127.0.0.1", backend_port, 1, {}});
        proxy->add_route(route);
        auto ws_proxy = std::make_shared<yuan::net::websocket::WebSocketProxy>(proxy, &svc.server());
        svc.server().set_ws_proxy_handler(
            [ws_proxy](yuan::net::AsyncConnectionContext ctx,
                       const std::string &url,
                       const std::string &route_key,
                       const std::string &client_key,
                       const std::string &subproto,
                       const std::string &origin,
                       ::yuan::buffer::ByteBuffer leftover) -> yuan::coroutine::Task<void> {
                co_await ws_proxy->proxy_connection(std::move(ctx), url, route_key, client_key, subproto, origin, std::move(leftover));
            });
        return true;
    });
    if (!service.init()) {
        std::cerr << "http websocket proxy service init failed\n";
        backend_stop.store(true);
        if (socket_t wake = connect_loopback(backend_port); wake != kInvalidSocket) {
            close_socket(wake);
        }
        if (backend_thread.joinable()) {
            backend_thread.join();
        }
        return 1;
    }

    check(service.server().has_ws_proxy_handler(), "http server should install websocket proxy handler");
    service.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    test_websocket_proxy_echo(proxy_port, saw_masked_backend_frame, saw_forwarded_headers);
    test_invalid_websocket_key_rejected(proxy_port);

    service.stop();
    backend_stop.store(true);
    if (socket_t wake = connect_loopback(backend_port); wake != kInvalidSocket) {
        close_socket(wake);
    }
    if (backend_thread.joinable()) {
        backend_thread.join();
    }

    const int exit_code = g_failed > 0 ? 1 : 0;
    if (exit_code == 0) {
        std::cout << "http websocket proxy tests passed\n";
    } else {
        std::cerr << "http websocket proxy tests failed=" << g_failed << '\n';
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return exit_code;
}
