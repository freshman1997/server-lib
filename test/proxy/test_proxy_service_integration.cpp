
#include "proxy_service.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <WS2tcpip.h>
#include <winsock2.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif

namespace
{
    int g_passed = 0;
    int g_failed = 0;

    void check(bool condition, const std::string &message)
    {
        if (condition) {
            ++g_passed;
        } else {
            ++g_failed;
            std::cerr << "  FAIL: " << message << '\n';
        }
    }

    void close_socket(socket_t sock)
    {
        if (sock == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        ::closesocket(sock);
#else
        ::close(sock);
#endif
    }

    bool set_socket_timeout(socket_t sock, int timeout_ms, int option)
    {
        if (sock == kInvalidSocket || timeout_ms < 0) {
            return false;
        }
#ifdef _WIN32
        const DWORD timeout = static_cast<DWORD>(timeout_ms);
        return ::setsockopt(sock, SOL_SOCKET, option, reinterpret_cast<const char *>(&timeout), sizeof(timeout)) == 0;
#else
        timeval timeout{};
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        return ::setsockopt(sock, SOL_SOCKET, option, &timeout, sizeof(timeout)) == 0;
#endif
    }

    bool shutdown_socket_write(socket_t sock)
    {
        if (sock == kInvalidSocket) {
            return false;
        }
#ifdef _WIN32
        return ::shutdown(sock, SD_SEND) == 0;
#else
        return ::shutdown(sock, SHUT_WR) == 0;
#endif
    }

    uint16_t reserve_tcp_port()
    {
        socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock == kInvalidSocket) {
            return 0;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(sock, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(sock);
            return 0;
        }

        sockaddr_in bound{};
#ifdef _WIN32
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        if (::getsockname(sock, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            close_socket(sock);
            return 0;
        }

        close_socket(sock);
        return ntohs(bound.sin_port);
    }

    bool send_all(socket_t sock, const std::string &data)
    {
        std::size_t sent = 0;
        while (sent < data.size()) {
#ifdef _WIN32
            const int rc = ::send(sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
            const ssize_t rc = ::send(sock, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#endif
            if (rc <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(rc);
        }
        return true;
    }

    std::string recv_some(socket_t sock)
    {
        std::string data;
        char buf[4096];
        while (true) {
#ifdef _WIN32
            const int rc = ::recv(sock, buf, static_cast<int>(sizeof(buf)), 0);
#else
            const ssize_t rc = ::recv(sock, buf, sizeof(buf), 0);
#endif
            if (rc <= 0) {
                break;
            }
            data.append(buf, static_cast<std::size_t>(rc));
        }
        return data;
    }

    std::string recv_exact(socket_t sock, std::size_t expected)
    {
        std::string data;
        data.reserve(expected);
        char buf[4096];
        while (data.size() < expected) {
#ifdef _WIN32
            const int rc = ::recv(sock, buf, static_cast<int>(sizeof(buf)), 0);
#else
            const ssize_t rc = ::recv(sock, buf, sizeof(buf), 0);
#endif
            if (rc <= 0) {
                break;
            }
            data.append(buf, static_cast<std::size_t>(rc));
        }
        return data;
    }

    std::string recv_with_grace(socket_t sock, std::size_t expected, int grace_ms)
    {
        if (sock == kInvalidSocket) {
            return {};
        }

        const int previous_timeout_ms = 3000;
        (void)set_socket_timeout(sock, grace_ms, SO_RCVTIMEO);
        std::string data = recv_exact(sock, expected);
        (void)set_socket_timeout(sock, previous_timeout_ms, SO_RCVTIMEO);
        return data;
    }

    bool wait_for_contains(socket_t sock, std::string &buffer, std::string_view needle, int max_reads = 16)
    {
        char chunk[1024];
        for (int i = 0; i < max_reads; ++i) {
            if (buffer.find(needle) != std::string::npos) {
                return true;
            }
#ifdef _WIN32
            const int rc = ::recv(sock, chunk, static_cast<int>(sizeof(chunk)), 0);
#else
            const ssize_t rc = ::recv(sock, chunk, sizeof(chunk), 0);
#endif
            if (rc <= 0) {
                break;
            }
            buffer.append(chunk, static_cast<std::size_t>(rc));
        }
        return buffer.find(needle) != std::string::npos;
    }

    socket_t connect_loopback(uint16_t port)
    {
        socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock == kInvalidSocket) {
            return kInvalidSocket;
        }

        (void)set_socket_timeout(sock, 3000, SO_RCVTIMEO);
        (void)set_socket_timeout(sock, 3000, SO_SNDTIMEO);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(sock, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(sock);
            return kInvalidSocket;
        }
        return sock;
    }

    void run_single_echo_server(uint16_t port,
                                std::atomic_bool &ready,
                                std::atomic_size_t *bytes_received = nullptr,
                                std::atomic_size_t *bytes_sent = nullptr)
    {
        socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener == kInvalidSocket) {
            return;
        }

        int reuse = 1;
#ifdef _WIN32
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#else
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0 ||
            ::listen(listener, 8) != 0) {
            close_socket(listener);
            return;
        }

        ready.store(true);
        socket_t client = ::accept(listener, nullptr, nullptr);
        if (client != kInvalidSocket) {
            char buf[2048];
            while (true) {
#ifdef _WIN32
                const int rc = ::recv(client, buf, static_cast<int>(sizeof(buf)), 0);
#else
                const ssize_t rc = ::recv(client, buf, sizeof(buf), 0);
#endif
                if (rc <= 0) {
                    break;
                }
                if (bytes_received) {
                    bytes_received->fetch_add(static_cast<std::size_t>(rc));
                }
                const std::string chunk(buf, static_cast<std::size_t>(rc));
                if (!send_all(client, chunk)) {
                    break;
                }
                if (bytes_sent) {
                    bytes_sent->fetch_add(static_cast<std::size_t>(rc));
                }
            }
            close_socket(client);
        }

        close_socket(listener);
    }

    void run_single_http_server(uint16_t port, std::atomic_bool &ready, std::string body)
    {
        socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener == kInvalidSocket) {
            return;
        }

        int reuse = 1;
#ifdef _WIN32
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#else
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0 ||
            ::listen(listener, 8) != 0) {
            close_socket(listener);
            return;
        }

        ready.store(true);
        socket_t client = ::accept(listener, nullptr, nullptr);
        if (client != kInvalidSocket) {
            std::string request;
            char buf[1024];
            while (request.find("\r\n\r\n") == std::string::npos) {
#ifdef _WIN32
                const int rc = ::recv(client, buf, static_cast<int>(sizeof(buf)), 0);
#else
                const ssize_t rc = ::recv(client, buf, sizeof(buf), 0);
#endif
                if (rc <= 0) {
                    break;
                }
                request.append(buf, static_cast<std::size_t>(rc));
            }

            const std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
            (void)send_all(client, response);
            close_socket(client);
        }

        close_socket(listener);
    }

    void wait_until_ready(const std::atomic_bool &ready)
    {
        for (int i = 0; i < 100 && !ready.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void test_proxy_connect_tunnel()
    {
        std::cout << "  [ProxyService] CONNECT tunnel echo\n";

        const uint16_t upstream_port = reserve_tcp_port();
        const uint16_t proxy_port = reserve_tcp_port();
        check(upstream_port != 0, "should reserve upstream echo port");
        check(proxy_port != 0, "should reserve proxy port");

        std::atomic_bool upstream_ready{ false };
        std::thread upstream_thread([&]() {
            run_single_echo_server(upstream_port, upstream_ready);
        });
        wait_until_ready(upstream_ready);
        check(upstream_ready.load(), "echo server should start");

        yuan::server::ProxyServiceConfig config;
        config.listen_host = "127.0.0.1";
        config.port = proxy_port;
        config.header_timeout_ms = 3000;
        config.idle_timeout_ms = 3000;
        config.connect_timeout_ms = 3000;
        config.drain_timeout_ms = 1000;
        config.allow_private_targets = true;

        auto service = std::make_unique<yuan::server::ProxyService>(config);
        check(service->init(), "proxy service init should succeed");
        service->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        socket_t client = connect_loopback(proxy_port);
        check(client != kInvalidSocket, "client should connect to proxy");
        if (client != kInvalidSocket) {
            const std::string connect_request =
                "CONNECT 127.0.0.1:" + std::to_string(upstream_port) + " HTTP/1.1\r\n"
                "Host: 127.0.0.1:" + std::to_string(upstream_port) + "\r\n"
                "Connection: keep-alive\r\n\r\n";
            check(send_all(client, connect_request), "CONNECT request should send");

            std::string response;
            check(wait_for_contains(client, response, "\r\n\r\n"), "CONNECT response should finish headers");
            check(response.find("200 Connection Established") != std::string::npos,
                  "proxy should establish CONNECT tunnel");
            if (response.find("200 Connection Established") == std::string::npos) {
                std::cerr << "  CONNECT raw response: [" << response << "]\n";
            }

            const std::string payload = "proxy-connect-payload";
            check(send_all(client, payload), "tunnel payload should send");

            std::string echoed;
            check(wait_for_contains(client, echoed, payload), "echo payload should arrive through tunnel");
            check(echoed.find(payload) != std::string::npos, "tunnel should echo payload exactly");

            close_socket(client);
        }

        service->stop();
        if (upstream_thread.joinable()) {
            upstream_thread.join();
        }
    }

    void test_proxy_plain_http_forward()
    {
        std::cout << "  [ProxyService] plain HTTP forward\n";

        const uint16_t upstream_port = reserve_tcp_port();
        const uint16_t proxy_port = reserve_tcp_port();
        check(upstream_port != 0, "should reserve upstream HTTP port");
        check(proxy_port != 0, "should reserve proxy port");

        std::atomic_bool upstream_ready{ false };
        const std::string body = "proxy-http-forward-ok";
        std::thread upstream_thread([&]() {
            run_single_http_server(upstream_port, upstream_ready, body);
        });
        wait_until_ready(upstream_ready);
        check(upstream_ready.load(), "HTTP server should start");

        yuan::server::ProxyServiceConfig config;
        config.listen_host = "127.0.0.1";
        config.port = proxy_port;
        config.header_timeout_ms = 3000;
        config.idle_timeout_ms = 3000;
        config.connect_timeout_ms = 3000;
        config.drain_timeout_ms = 1000;
        config.allow_private_targets = true;

        auto service = std::make_unique<yuan::server::ProxyService>(config);
        check(service->init(), "proxy service init should succeed");
        service->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        socket_t client = connect_loopback(proxy_port);
        check(client != kInvalidSocket, "client should connect to proxy");
        if (client != kInvalidSocket) {
            const std::string request =
                "GET http://127.0.0.1:" + std::to_string(upstream_port) + "/status HTTP/1.1\r\n"
                "Host: 127.0.0.1:" + std::to_string(upstream_port) + "\r\n"
                "Connection: close\r\n\r\n";
            check(send_all(client, request), "HTTP proxy request should send");

            const std::string response = recv_some(client);
            check(response.find("HTTP/1.1 200 OK") != std::string::npos,
                  "proxy should forward HTTP response status");
            check(response.find(body) != std::string::npos,
                  "proxy should forward HTTP response body");
            if (response.find("HTTP/1.1 200 OK") == std::string::npos || response.find(body) == std::string::npos) {
                std::cerr << "  HTTP raw response: [" << response << "]\n";
            }

            close_socket(client);
        }

        service->stop();
        if (upstream_thread.joinable()) {
            upstream_thread.join();
        }
    }

    void test_proxy_large_http_forward()
    {
        std::cout << "  [ProxyService] large HTTP forward\n";

        const uint16_t upstream_port = reserve_tcp_port();
        const uint16_t proxy_port = reserve_tcp_port();
        check(upstream_port != 0, "should reserve upstream HTTP port for large response");
        check(proxy_port != 0, "should reserve proxy port for large response");

        std::atomic_bool upstream_ready{ false };
        const std::string body(256 * 1024, 'x');
        std::thread upstream_thread([&]() {
            run_single_http_server(upstream_port, upstream_ready, body);
        });
        wait_until_ready(upstream_ready);
        check(upstream_ready.load(), "large HTTP server should start");

        yuan::server::ProxyServiceConfig config;
        config.listen_host = "127.0.0.1";
        config.port = proxy_port;
        config.header_timeout_ms = 3000;
        config.idle_timeout_ms = 3000;
        config.connect_timeout_ms = 3000;
        config.drain_timeout_ms = 1000;
        config.allow_private_targets = true;

        auto service = std::make_unique<yuan::server::ProxyService>(config);
        check(service->init(), "proxy service init should succeed for large response");
        service->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        socket_t client = connect_loopback(proxy_port);
        check(client != kInvalidSocket, "client should connect to proxy for large response");
        if (client != kInvalidSocket) {
            const std::string request =
                "GET http://127.0.0.1:" + std::to_string(upstream_port) + "/large HTTP/1.1\r\n"
                "Host: 127.0.0.1:" + std::to_string(upstream_port) + "\r\n"
                "Connection: close\r\n\r\n";
            check(send_all(client, request), "large HTTP proxy request should send");

            const std::string response = recv_some(client);
            check(response.find("HTTP/1.1 200 OK") != std::string::npos,
                  "proxy should forward large HTTP response status");
            check(response.find("Content-Length: " + std::to_string(body.size())) != std::string::npos,
                  "proxy should preserve large response content-length");
            check(response.find(body.substr(0, 1024)) != std::string::npos,
                  "proxy should forward the beginning of the large response body");
            check(response.size() >= body.size(),
                  "proxy should forward the full large response payload");

            close_socket(client);
        }

        service->stop();
        if (upstream_thread.joinable()) {
            upstream_thread.join();
        }
    }

    void test_proxy_connect_large_tunnel_half_close_smoke()
    {
        std::cout << "  [ProxyService] CONNECT large tunnel half-close smoke\n";

        const uint16_t upstream_port = reserve_tcp_port();
        const uint16_t proxy_port = reserve_tcp_port();
        check(upstream_port != 0, "should reserve upstream echo port for half-close smoke");
        check(proxy_port != 0, "should reserve proxy port for half-close smoke");

        std::atomic_bool upstream_ready{ false };
        std::atomic_size_t upstream_received{ 0 };
        std::atomic_size_t upstream_sent{ 0 };
        std::thread upstream_thread([&]() {
            run_single_echo_server(upstream_port, upstream_ready, &upstream_received, &upstream_sent);
        });
        wait_until_ready(upstream_ready);
        check(upstream_ready.load(), "half-close smoke echo server should start");

        yuan::server::ProxyServiceConfig config;
        config.listen_host = "127.0.0.1";
        config.port = proxy_port;
        config.header_timeout_ms = 3000;
        config.idle_timeout_ms = 3000;
        config.connect_timeout_ms = 3000;
        config.drain_timeout_ms = 1000;
        config.allow_private_targets = true;

        auto service = std::make_unique<yuan::server::ProxyService>(config);
        check(service->init(), "proxy service init should succeed for half-close smoke");
        service->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        socket_t client = connect_loopback(proxy_port);
        check(client != kInvalidSocket, "client should connect to proxy for half-close smoke");
        if (client != kInvalidSocket) {
            const std::string connect_request =
                "CONNECT 127.0.0.1:" + std::to_string(upstream_port) + " HTTP/1.1\r\n"
                "Host: 127.0.0.1:" + std::to_string(upstream_port) + "\r\n"
                "Connection: keep-alive\r\n\r\n";
            check(send_all(client, connect_request), "half-close smoke CONNECT request should send");

            std::string response;
            check(wait_for_contains(client, response, "\r\n\r\n"), "half-close smoke CONNECT response should finish headers");
            check(response.find("200 Connection Established") != std::string::npos,
                  "proxy should establish half-close smoke CONNECT tunnel");

            const std::string payload(128 * 1024, 'p');
            check(send_all(client, payload), "half-close smoke payload should send");
            check(shutdown_socket_write(client), "client should half-close write side after smoke payload");

            const std::string echoed = recv_with_grace(client, payload.size(), 12000);
            const std::size_t expected_min = payload.size() / 3;
            check(echoed.size() >= expected_min,
                  "half-close smoke should echo a substantial payload prefix");
            check(payload.compare(0, echoed.size(), echoed) == 0,
                  "half-close smoke echoed payload prefix should match exactly");
            if (echoed.size() < expected_min || payload.compare(0, echoed.size(), echoed) != 0) {
                std::cerr << "  half-close smoke bytes: expected>=" << expected_min
                          << " actual=" << echoed.size() << '\n';
                if (!echoed.empty()) {
                    std::size_t mismatch = 0;
                    const std::size_t n = std::min(echoed.size(), payload.size());
                    while (mismatch < n && echoed[mismatch] == payload[mismatch]) {
                        ++mismatch;
                    }
                    std::cerr << "  half-close smoke first mismatch index=" << mismatch << '\n';
                }
            }

            close_socket(client);
        }

        service->stop();
        if (upstream_thread.joinable()) {
            upstream_thread.join();
        }

        check(upstream_received.load() > 0, "upstream should receive half-close smoke payload bytes");
        check(upstream_sent.load() > 0, "upstream should echo half-close smoke payload bytes");
    }

    void test_proxy_connect_large_tunnel_no_half_close()
    {
        std::cout << "  [ProxyService] CONNECT large tunnel no half-close\n";

        const uint16_t upstream_port = reserve_tcp_port();
        const uint16_t proxy_port = reserve_tcp_port();
        check(upstream_port != 0, "should reserve upstream echo port for strict tunnel");
        check(proxy_port != 0, "should reserve proxy port for strict tunnel");

        std::atomic_bool upstream_ready{ false };
        std::thread upstream_thread([&]() {
            run_single_echo_server(upstream_port, upstream_ready);
        });
        wait_until_ready(upstream_ready);
        check(upstream_ready.load(), "strict tunnel echo server should start");

        yuan::server::ProxyServiceConfig config;
        config.listen_host = "127.0.0.1";
        config.port = proxy_port;
        config.header_timeout_ms = 3000;
        config.idle_timeout_ms = 3000;
        config.connect_timeout_ms = 3000;
        config.drain_timeout_ms = 1000;
        config.allow_private_targets = true;

        auto service = std::make_unique<yuan::server::ProxyService>(config);
        check(service->init(), "proxy service init should succeed for strict tunnel");
        service->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        socket_t client = connect_loopback(proxy_port);
        check(client != kInvalidSocket, "client should connect to proxy for strict tunnel");
        if (client != kInvalidSocket) {
            const std::string connect_request =
                "CONNECT 127.0.0.1:" + std::to_string(upstream_port) + " HTTP/1.1\r\n"
                "Host: 127.0.0.1:" + std::to_string(upstream_port) + "\r\n"
                "Connection: keep-alive\r\n\r\n";
            check(send_all(client, connect_request), "strict CONNECT request should send");

            std::string response;
            check(wait_for_contains(client, response, "\r\n\r\n"), "strict CONNECT response should finish headers");
            check(response.find("200 Connection Established") != std::string::npos,
                  "proxy should establish strict CONNECT tunnel");

            const std::string payload(128 * 1024, 'q');
            check(send_all(client, payload), "strict tunnel payload should send");
            const std::string echoed = recv_with_grace(client, payload.size(), 12000);
            check(echoed.size() == payload.size(),
                  "strict tunnel should echo full payload before half-close");
            check(echoed == payload,
                  "strict tunnel echoed payload should match exactly before half-close");
            check(shutdown_socket_write(client), "strict tunnel should allow delayed half-close");
            close_socket(client);
        }

        service->stop();
        if (upstream_thread.joinable()) {
            upstream_thread.join();
        }
    }
}

int main()
{
    std::cout << "Running proxy service integration tests...\n\n";

    test_proxy_connect_tunnel();
    test_proxy_connect_large_tunnel_half_close_smoke();
    test_proxy_connect_large_tunnel_no_half_close();
    test_proxy_plain_http_forward();
    test_proxy_large_http_forward();

    std::cout << '\n';
    if (g_failed > 0) {
        std::cout << "FAILED: " << g_failed << " assertions failed\n";
        return 1;
    }

    std::cout << "All proxy service integration tests passed (" << g_passed << " assertions ok)\n";
    return 0;
}
