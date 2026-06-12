#include "common/websocket_protocol.h"
#include "common/websocket_utils.h"
#include "common/websocket_connection.h"
#include "entry/data_handler.h"
#include "entry/server.h"

#include "buffer/byte_buffer.h"
#include "net/runtime/network_runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

using namespace yuan::net::websocket;

namespace
{
    constexpr int SOCKET_TIMEOUT_MS = 2500;
    constexpr int SHORT_SOCKET_TIMEOUT_MS = 400;
    constexpr uint32_t TEST_HANDSHAKE_TIMEOUT_MS = 300;
    constexpr int STARTUP_WAIT_ATTEMPTS = 100;
    constexpr int STARTUP_WAIT_MS = 10;
    constexpr std::size_t MAX_HTTP_HEADER_BYTES = 16 * 1024;
    constexpr std::size_t MAX_FRAME_PAYLOAD_BYTES = PACKET_MAX_BYTE;
    constexpr uint8_t CLIENT_MASK_KEY[4] = {0x12, 0x34, 0x56, 0x78};
    constexpr const char *CLIENT_KEY = "dGhlIHNhbXBsZSBub25jZQ==";
    constexpr const char *WS_PATH = "/ws/e2e";
    constexpr const char *BROWSER_ORIGIN = "https://browser.example";
    constexpr int CONCURRENT_CLIENTS = 8;
    constexpr int MESSAGES_PER_CLIENT = 16;
    constexpr int DEFAULT_SOAK_SMOKE_ROUNDS = 12;
    constexpr int SOAK_MESSAGES_PER_RECONNECT = 3;
    constexpr const char *SOAK_SECONDS_ENV = "WEBSOCKET_SOAK_SECONDS";

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

    socket_t connect_loopback(uint16_t port, int timeout_ms = SOCKET_TIMEOUT_MS)
    {
        socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s == kInvalidSocket) {
            return kInvalidSocket;
        }
        set_socket_timeouts(s, timeout_ms);

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

    std::string build_frame(std::string_view payload, uint8_t opcode, bool masked, bool fin = true)
    {
        std::string out;
        out.push_back(static_cast<char>((fin ? 0x80U : 0U) | opcode));
        const std::size_t len = payload.size();
        const uint8_t mask_bit = masked ? 0x80U : 0U;
        if (len <= websocket_payload_len_7bit_max) {
            out.push_back(static_cast<char>(mask_bit | static_cast<uint8_t>(len)));
        } else if (len <= websocket_payload_len_16bit_max) {
            out.push_back(static_cast<char>(mask_bit | websocket_payload_len_16bit_marker));
            out.push_back(static_cast<char>((len >> 8) & 0xffU));
            out.push_back(static_cast<char>(len & 0xffU));
        } else {
            out.push_back(static_cast<char>(mask_bit | websocket_payload_len_64bit_marker));
            for (int shift = 56; shift >= 0; shift -= 8) {
                out.push_back(static_cast<char>((static_cast<uint64_t>(len) >> shift) & 0xffU));
            }
        }

        if (masked) {
            out.append(reinterpret_cast<const char *>(CLIENT_MASK_KEY), 4);
            for (std::size_t i = 0; i < payload.size(); ++i) {
                out.push_back(static_cast<char>(static_cast<uint8_t>(payload[i]) ^ CLIENT_MASK_KEY[i % 4]));
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

        uint64_t len = first_two[1] & 0x7fU;
        if (len == websocket_payload_len_16bit_marker) {
            uint8_t ext[2]{};
            if (!recv_exact(s, reinterpret_cast<char *>(ext), sizeof(ext))) {
                return std::nullopt;
            }
            len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (len == websocket_payload_len_64bit_marker) {
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

    std::string websocket_handshake_request(uint16_t port)
    {
        return "GET " + std::string(WS_PATH) + " HTTP/1.1\r\n"
               "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
               "Upgrade: websocket\r\n"
               "Connection: keep-alive, Upgrade\r\n"
               "Sec-WebSocket-Key: " + std::string(CLIENT_KEY) + "\r\n"
               "Sec-WebSocket-Version: 13\r\n"
               "Origin: " + std::string(BROWSER_ORIGIN) + "\r\n\r\n";
    }

    bool open_browser_like_websocket(uint16_t port, socket_t &client)
    {
        client = connect_loopback(port);
        if (client == kInvalidSocket) {
            return false;
        }
        if (!send_all(client, websocket_handshake_request(port))) {
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
        const std::string expected_accept = WebSocketUtils::generate_server_key(CLIENT_KEY);
        return lower.find("101 switching protocols") != std::string::npos &&
               lower.find("connection: upgrade") != std::string::npos &&
               lower.find("upgrade: websocket") != std::string::npos &&
               response.find(expected_accept) != std::string::npos;
    }

    class EchoHandler : public WebSocketDataHandler
    {
    public:
        void on_connected(WebSocketConnection *) override
        {
            connected.fetch_add(1, std::memory_order_relaxed);
        }

        void on_data(WebSocketConnection *wsConn, const yuan::buffer::ByteBuffer &buff) override
        {
            messages.fetch_add(1, std::memory_order_relaxed);
            bytes.fetch_add(buff.readable_bytes(), std::memory_order_relaxed);
            wsConn->send(buff, WebSocketConnection::PacketType::binary_);
        }

        void on_close(WebSocketConnection *) override
        {
            closed.fetch_add(1, std::memory_order_relaxed);
        }

        std::atomic_int connected{0};
        std::atomic_int messages{0};
        std::atomic<std::size_t> bytes{0};
        std::atomic_int closed{0};
    };

    struct ServerHarness
    {
        explicit ServerHarness(uint16_t p)
            : port(p)
        {
        }

        bool start()
        {
            std::ofstream config("ws_cfg.json");
            config << "{\n"
                   << "  \"handshake_timeout\": " << TEST_HANDSHAKE_TIMEOUT_MS << ",\n"
                   << "  \"read_idle_timeout\": 2000,\n"
                   << "  \"heat_beat_timeout\": 1000,\n"
                   << "  \"heart_beat_interval\": 0\n"
                   << "}\n";
            config.close();

            server.set_data_handler(&handler);
            server.set_origin_validator([](std::string_view origin) {
                return origin == BROWSER_ORIGIN;
            });
            if (!server.init(port, runtime)) {
                return false;
            }
            server.serve();
            thread = std::thread([this]() {
                runtime.run();
            });

            for (int i = 0; i < STARTUP_WAIT_ATTEMPTS; ++i) {
                socket_t probe = connect_loopback(port, SHORT_SOCKET_TIMEOUT_MS);
                if (probe != kInvalidSocket) {
                    close_socket(probe);
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(STARTUP_WAIT_MS));
            }
            return false;
        }

        void stop()
        {
            server.stop();
            runtime.stop();
            if (thread.joinable()) {
                thread.join();
            }
            std::remove("ws_cfg.json");
        }

        uint16_t port;
        yuan::net::NetworkRuntime runtime;
        WebSocketServer server;
        EchoHandler handler;
        std::thread thread;
    };

    bool send_and_expect_echo(socket_t client, std::string_view payload, uint8_t opcode)
    {
        if (!send_all(client, build_frame(payload, opcode, true))) {
            return false;
        }
        auto reply = recv_frame(client);
        return reply.has_value() && !reply->masked && reply->payload == payload;
    }

    void test_browser_compatibility_smoke(uint16_t port)
    {
        socket_t client = kInvalidSocket;
        check(open_browser_like_websocket(port, client), "browser-like client should complete RFC 6455 handshake");
        if (client == kInvalidSocket) {
            return;
        }

        check(send_all(client, build_frame("hello-browser", static_cast<uint8_t>(OpCodeType::type_text_frame), true)),
              "browser-like client should send masked text frame");
        auto text_reply = recv_frame(client);
        check(text_reply.has_value(), "browser-like client should receive text echo");
        if (text_reply) {
            check(!text_reply->masked, "server frame should be unmasked for browser client");
            check(text_reply->opcode == static_cast<uint8_t>(OpCodeType::type_binary_frame),
                  "test echo handler should preserve payload through binary send path");
            check(text_reply->payload == "hello-browser", "text payload should round-trip");
        }

        const std::string binary_payload{"\x00\x01\x02\xff", 4};
        check(send_all(client, build_frame(binary_payload, static_cast<uint8_t>(OpCodeType::type_binary_frame), true)),
              "browser-like client should send masked binary frame");
        auto binary_reply = recv_frame(client);
        check(binary_reply.has_value(), "browser-like client should receive binary echo");
        if (binary_reply) {
            check(binary_reply->payload == binary_payload, "binary payload should round-trip");
        }

        check(send_all(client, build_frame("ping-body", static_cast<uint8_t>(OpCodeType::type_ping_frame), true)),
              "browser-like client should send masked ping");
        auto pong = recv_frame(client);
        check(pong.has_value(), "browser-like client should receive pong");
        if (pong) {
            check(pong->opcode == static_cast<uint8_t>(OpCodeType::type_pong_frame), "ping should receive pong opcode");
            check(pong->payload == "ping-body", "pong payload should match ping payload");
        }

        check(send_all(client, build_frame("", static_cast<uint8_t>(OpCodeType::type_close_frame), true)),
              "browser-like client should send close frame");
        auto close_reply = recv_frame(client);
        check(close_reply.has_value(), "browser-like client should receive close reply");
        if (close_reply) {
            check(close_reply->opcode == static_cast<uint8_t>(OpCodeType::type_close_frame), "server should reply with close opcode");
        }
        close_socket(client);
    }

    void test_slow_handshake_timeout(uint16_t port)
    {
        socket_t client = connect_loopback(port, SHORT_SOCKET_TIMEOUT_MS);
        check(client != kInvalidSocket, "slow-handshake client should connect");
        if (client == kInvalidSocket) {
            return;
        }

        check(send_all(client, "GET /ws/e2e HTTP/1.1\r\nHost: 127.0.0.1\r\n"),
              "slow-handshake client should send partial headers");
        std::this_thread::sleep_for(std::chrono::milliseconds(TEST_HANDSHAKE_TIMEOUT_MS + 250));

        std::string response;
        const bool completed = recv_until_http_headers(client, response, 512);
        check(!completed, "slow incomplete handshake should be closed without upgrade response");
        close_socket(client);
    }

    void test_concurrent_message_throughput(uint16_t port)
    {
        const auto start = std::chrono::steady_clock::now();
        std::atomic_int ok_clients{0};
        std::vector<std::thread> clients;
        clients.reserve(CONCURRENT_CLIENTS);

        for (int client_index = 0; client_index < CONCURRENT_CLIENTS; ++client_index) {
            clients.emplace_back([port, client_index, &ok_clients]() {
                socket_t client = kInvalidSocket;
                if (!open_browser_like_websocket(port, client)) {
                    return;
                }

                bool ok = true;
                for (int message_index = 0; message_index < MESSAGES_PER_CLIENT; ++message_index) {
                    const std::string payload = "client-" + std::to_string(client_index) + "-message-" + std::to_string(message_index);
                    if (!send_all(client, build_frame(payload, static_cast<uint8_t>(OpCodeType::type_binary_frame), true))) {
                        ok = false;
                        break;
                    }
                    auto reply = recv_frame(client);
                    if (!reply || reply->payload != payload) {
                        ok = false;
                        break;
                    }
                }
                (void)send_all(client, build_frame("", static_cast<uint8_t>(OpCodeType::type_close_frame), true));
                close_socket(client);
                if (ok) {
                    ok_clients.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (auto &client : clients) {
            client.join();
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        const int total_messages = CONCURRENT_CLIENTS * MESSAGES_PER_CLIENT;
        check(ok_clients.load(std::memory_order_relaxed) == CONCURRENT_CLIENTS,
              "all concurrent websocket clients should complete echo workload");
        check(elapsed < std::chrono::seconds(5), "concurrent websocket echo workload should complete within benchmark budget");
        std::cout << "websocket throughput smoke: messages=" << total_messages
                  << ", clients=" << CONCURRENT_CLIENTS
                  << ", elapsed_ms=" << elapsed.count() << '\n';
    }

    int parse_soak_seconds()
    {
        const char *raw = std::getenv(SOAK_SECONDS_ENV);
        if (!raw || *raw == '\0') {
            return 0;
        }

        char *end = nullptr;
        const long value = std::strtol(raw, &end, 10);
        if (!end || *end != '\0' || value <= 0) {
            return 0;
        }
        return static_cast<int>(value);
    }

    void test_reconnect_mixed_payload_soak(uint16_t port)
    {
        const int soak_seconds = parse_soak_seconds();
        const auto deadline = soak_seconds > 0
                                  ? std::chrono::steady_clock::now() + std::chrono::seconds(soak_seconds)
                                  : std::chrono::steady_clock::time_point::max();
        const bool long_mode = soak_seconds > 0;
        std::size_t total_bytes = 0;
        int rounds = 0;
        while ((long_mode && std::chrono::steady_clock::now() < deadline) ||
               (!long_mode && rounds < DEFAULT_SOAK_SMOKE_ROUNDS)) {
            socket_t client = kInvalidSocket;
            check(open_browser_like_websocket(port, client), "soak-smoke client should reconnect");
            if (client == kInvalidSocket) {
                ++rounds;
                continue;
            }

            const std::string small = "soak-small-" + std::to_string(rounds);
            const std::string medium(256 + static_cast<std::size_t>(rounds), static_cast<char>('a' + (rounds % 26)));
            const std::string large(4096 + static_cast<std::size_t>(rounds * 17), static_cast<char>('A' + (rounds % 26)));

            const bool ok = send_and_expect_echo(client, small, static_cast<uint8_t>(OpCodeType::type_text_frame)) &&
                            send_and_expect_echo(client, medium, static_cast<uint8_t>(OpCodeType::type_binary_frame)) &&
                            send_and_expect_echo(client, large, static_cast<uint8_t>(OpCodeType::type_binary_frame));
            check(ok, "soak-smoke mixed payloads should round-trip after reconnect");
            total_bytes += small.size() + medium.size() + large.size();
            (void)send_all(client, build_frame("", static_cast<uint8_t>(OpCodeType::type_close_frame), true));
            close_socket(client);
            ++rounds;
        }

        std::cout << "websocket soak " << (long_mode ? "long" : "smoke")
                  << ": reconnects=" << rounds
                  << ", mixed_payload_bytes=" << total_bytes;
        if (long_mode) {
            std::cout << ", requested_seconds=" << soak_seconds;
        }
        std::cout << '\n';
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

    const uint16_t port = reserve_tcp_port();
    check(port != 0, "should reserve websocket e2e port");
    if (port == 0) {
        return 1;
    }

    ServerHarness harness(port);
    check(harness.start(), "websocket e2e server should start");
    if (g_failed == 0) {
        test_browser_compatibility_smoke(port);
        test_slow_handshake_timeout(port);
        test_concurrent_message_throughput(port);
        test_reconnect_mixed_payload_soak(port);
    }
    harness.stop();

    const int expected_soak_rounds = parse_soak_seconds() > 0 ? 1 : DEFAULT_SOAK_SMOKE_ROUNDS;
    const int expected_messages = 2 + CONCURRENT_CLIENTS * MESSAGES_PER_CLIENT + expected_soak_rounds * SOAK_MESSAGES_PER_RECONNECT;
    check(harness.handler.messages.load(std::memory_order_relaxed) >= expected_messages,
          "server handler should observe browser smoke and throughput messages");

    const int exit_code = g_failed > 0 ? 1 : 0;
    if (exit_code == 0) {
        std::cout << "websocket e2e tests passed\n";
    } else {
        std::cerr << "websocket e2e tests failed=" << g_failed << '\n';
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return exit_code;
}
