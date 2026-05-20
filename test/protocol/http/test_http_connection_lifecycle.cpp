#include "http_service.h"
#include "ops/option.h"
#include "response.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifdef HTTP_USE_SSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
    int g_failed = 0;

#ifdef _WIN32
    using socket_t = SOCKET;
    constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
    using socket_t = int;
    constexpr socket_t kInvalidSocket = -1;
#endif

    void check(bool cond, const char *message)
    {
        if (!cond) {
            ++g_failed;
            std::cerr << "[FAIL] " << message << '\n';
        }
    }

    void close_socket(socket_t fd)
    {
        if (fd == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        ::closesocket(fd);
#else
        ::close(fd);
#endif
    }

    void shutdown_send(socket_t fd)
    {
#ifdef _WIN32
        (void)::shutdown(fd, SD_SEND);
#else
        (void)::shutdown(fd, SHUT_WR);
#endif
    }

    void set_timeouts(socket_t fd, int ms)
    {
#ifdef _WIN32
        const DWORD timeout = static_cast<DWORD>(ms);
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
        timeval tv{};
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    }

    bool send_all(socket_t fd, const std::string &data)
    {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const int rc = ::send(fd, data.data() + sent, static_cast<int>(data.size() - sent), 0);
            if (rc <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(rc);
        }
        return true;
    }

    socket_t connect_loopback(uint16_t port)
    {
        socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == kInvalidSocket) {
            return kInvalidSocket;
        }
        set_timeouts(fd, 2000);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(fd);
            return kInvalidSocket;
        }
        return fd;
    }

    uint16_t reserve_tcp_port()
    {
        socket_t listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
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

    std::string recv_until(socket_t fd, std::string_view marker, bool *saw_eof = nullptr)
    {
        if (saw_eof) {
            *saw_eof = false;
        }

        std::string out;
        char buf[2048];
        for (;;) {
            const int rc = ::recv(fd, buf, sizeof(buf), 0);
            if (rc > 0) {
                out.append(buf, static_cast<std::size_t>(rc));
                if (!marker.empty() && out.find(marker) != std::string::npos) {
                    return out;
                }
                continue;
            }
            if (rc == 0 && saw_eof) {
                *saw_eof = true;
            }
            return out;
        }
    }

    std::string http_get(uint16_t port, const std::string &path, bool half_close, bool *saw_eof = nullptr)
    {
        socket_t fd = connect_loopback(port);
        if (fd == kInvalidSocket) {
            return {};
        }

        const std::string req =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
            "Connection: keep-alive\r\n\r\n";
        if (!send_all(fd, req)) {
            close_socket(fd);
            return {};
        }
        if (half_close) {
            shutdown_send(fd);
        }

        auto response = recv_until(fd, "ok", saw_eof);
        if (half_close && saw_eof && !*saw_eof) {
            const auto tail = recv_until(fd, {}, saw_eof);
            response += tail;
        }
        close_socket(fd);
        return response;
    }

    bool wait_for_tcp(uint16_t port)
    {
        for (int i = 0; i < 80; ++i) {
            socket_t fd = connect_loopback(port);
            if (fd != kInvalidSocket) {
                close_socket(fd);
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return false;
    }

    bool wait_for_zero_connections(yuan::server::HttpService &service)
    {
        for (int i = 0; i < 80; ++i) {
            if (service.server().snapshot_server_stats().active_http_connections == 0) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return service.server().snapshot_server_stats().active_http_connections == 0;
    }

    void install_ok_route(yuan::server::HttpService &service, const std::string &body)
    {
        service.server().on("/ok",
            [body](yuan::net::http::HttpRequest *, yuan::net::http::HttpResponse *resp) {
                resp->set_response_code(yuan::net::http::ResponseCode::ok_);
                resp->add_header("Content-Type", "text/plain");
                resp->add_header("Content-Length", std::to_string(body.size()));
                resp->append_body(body);
                resp->send();
            });
    }

#ifdef HTTP_USE_SSL
    std::string https_get(uint16_t port)
    {
        socket_t fd = connect_loopback(port);
        if (fd == kInvalidSocket) {
            return {};
        }

        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) {
            close_socket(fd);
            return {};
        }
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

        SSL *ssl = SSL_new(ctx);
        if (!ssl) {
            SSL_CTX_free(ctx);
            close_socket(fd);
            return {};
        }

        SSL_set_fd(ssl, static_cast<int>(fd));
        std::string response;
        if (SSL_connect(ssl) == 1) {
            const std::string req =
                "GET /ok HTTP/1.1\r\n"
                "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
                "Connection: close\r\n\r\n";
            if (SSL_write(ssl, req.data(), static_cast<int>(req.size())) > 0) {
                char buf[2048];
                for (;;) {
                    const int rc = SSL_read(ssl, buf, sizeof(buf));
                    if (rc <= 0) {
                        break;
                    }
                    response.append(buf, static_cast<std::size_t>(rc));
                    if (response.find("secure-ok") != std::string::npos) {
                        break;
                    }
                }
            }
        }

        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close_socket(fd);
        return response;
    }
#endif
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

    yuan::net::http::config::connection_idle_timeout = 500;

    const uint16_t http_port = reserve_tcp_port();
    check(http_port != 0, "should reserve HTTP port");

    yuan::net::http::HttpServerConfig http_cfg;
    http_cfg.enable_ssl = false;
    http_cfg.enable_keep_alive = true;
    yuan::server::HttpService http_service(http_port, http_cfg);
    install_ok_route(http_service, "ok");
    check(http_service.init(), "HTTP service should init");
    http_service.start();
    check(wait_for_tcp(http_port), "HTTP service should accept TCP");

    for (int i = 0; i < 32; ++i) {
        bool saw_eof = false;
        const auto response = http_get(http_port, "/ok", true, &saw_eof);
        check(response.find("200") != std::string::npos, "half-close HTTP request should receive 200");
        check(response.find("ok") != std::string::npos, "half-close HTTP request should receive body");
        check(saw_eof, "server should close after responding to peer half-close");
    }
    check(wait_for_zero_connections(http_service), "HTTP half-close connections should drain");

#ifdef HTTP_USE_SSL
    const uint16_t https_port = reserve_tcp_port();
    check(https_port != 0, "should reserve HTTPS port");

    yuan::net::http::HttpServerConfig https_cfg;
    https_cfg.enable_ssl = true;
    https_cfg.enable_keep_alive = true;
    yuan::server::HttpService https_service(https_port, https_cfg);
    install_ok_route(https_service, "secure-ok");
    check(https_service.init(), "HTTPS service should init");
    https_service.start();
    check(wait_for_tcp(https_port), "HTTPS service should accept TCP");

    for (int i = 0; i < 8; ++i) {
        bool saw_eof = false;
        const auto response = http_get(http_port, "/ok", true, &saw_eof);
        check(response.find("200") != std::string::npos, "mixed HTTP request should receive 200");
        check(saw_eof, "mixed HTTP half-close should drain");
    }

    const auto secure_response = https_get(https_port);
    check(secure_response.find("200") != std::string::npos, "HTTPS request should receive 200");
    check(secure_response.find("secure-ok") != std::string::npos, "HTTPS request should receive body");

    socket_t bad_tls = connect_loopback(https_port);
    check(bad_tls != kInvalidSocket, "bad TLS client should connect");
    if (bad_tls != kInvalidSocket) {
        (void)send_all(bad_tls, "GET /ok HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
        close_socket(bad_tls);
    }

    check(wait_for_zero_connections(https_service), "HTTPS connections should drain");
    https_service.stop();
#else
    yuan::net::http::HttpServerConfig https_cfg;
    https_cfg.enable_ssl = true;
    yuan::server::HttpService https_service(static_cast<int>(reserve_tcp_port()), https_cfg);
    check(!https_service.init(), "HTTPS service should fail fast when HTTP SSL is disabled");
#endif

    check(wait_for_zero_connections(http_service), "HTTP connections should still be drained after mixed run");
    http_service.stop();

#ifdef _WIN32
    WSACleanup();
#endif

    if (g_failed != 0) {
        std::cout << "FAILED: " << g_failed << " assertions failed\n";
        return 1;
    }

    std::cout << "HTTP connection lifecycle regression passed\n";
    return 0;
}
