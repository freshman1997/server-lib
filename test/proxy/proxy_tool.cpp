#include "socks5.h"
#include "base/utils/base64.h"
#include "logger.h"
#include "net/socket/socket.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t invalid_socket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t invalid_socket = -1;
#endif

namespace
{
    using SocketHandle = std::unique_ptr<yuan::net::Socket>;

    std::atomic_bool g_stop{ false };
    std::atomic_int g_active_http_sessions{ 0 };

    struct HttpProxySettings
    {
        int max_active_sessions = 128;
        int header_timeout_ms = 15000;
        int idle_timeout_ms = 300000;
        int connect_timeout_ms = 10000;
        bool require_basic_auth = false;
        std::string auth_user;
        std::string auth_password;
    };

    struct SessionGuard
    {
        explicit SessionGuard(std::atomic_int &counter) : counter_(counter)
        {
            counter_.fetch_add(1, std::memory_order_relaxed);
        }

        ~SessionGuard()
        {
            counter_.fetch_sub(1, std::memory_order_relaxed);
        }

        std::atomic_int &counter_;
    };

    void on_signal(int)
    {
        g_stop.store(true, std::memory_order_relaxed);
    }

    void close_socket(socket_t sock)
    {
        if (sock == invalid_socket) {
            return;
        }
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
    }

    void shutdown_socket(socket_t sock)
    {
        if (sock == invalid_socket) {
            return;
        }
#ifdef _WIN32
        (void)::shutdown(sock, SD_BOTH);
#else
        (void)::shutdown(sock, SHUT_RDWR);
#endif
    }

    std::string socket_error_message()
    {
#ifdef _WIN32
        return "WSA error " + std::to_string(::WSAGetLastError());
#else
        return std::strerror(errno);
#endif
    }

    int read_env_int(const char *name, int default_value)
    {
        const char *raw = std::getenv(name);
        if (!raw || *raw == '\0') {
            return default_value;
        }
        try {
            return std::stoi(raw);
        } catch (...) {
            return default_value;
        }
    }

    std::string read_env_string(const char *name)
    {
        const char *raw = std::getenv(name);
        return raw ? std::string(raw) : std::string{};
    }

    HttpProxySettings load_http_proxy_settings()
    {
        HttpProxySettings settings;
        settings.max_active_sessions = std::max(1, read_env_int("YUAN_PROXY_MAX_ACTIVE", settings.max_active_sessions));
        settings.header_timeout_ms = std::max(1000, read_env_int("YUAN_PROXY_HEADER_TIMEOUT_MS", settings.header_timeout_ms));
        settings.idle_timeout_ms = std::max(1000, read_env_int("YUAN_PROXY_IDLE_TIMEOUT_MS", settings.idle_timeout_ms));
        settings.connect_timeout_ms = std::max(1000, read_env_int("YUAN_PROXY_CONNECT_TIMEOUT_MS", settings.connect_timeout_ms));
        settings.auth_user = read_env_string("YUAN_PROXY_BASIC_USER");
        settings.auth_password = read_env_string("YUAN_PROXY_BASIC_PASS");
        settings.require_basic_auth = !settings.auth_user.empty();
        return settings;
    }

    bool set_socket_timeout(socket_t sock, int timeout_ms, int optname)
    {
#ifdef _WIN32
        return ::setsockopt(sock, SOL_SOCKET, optname,
                            reinterpret_cast<const char *>(&timeout_ms),
                            static_cast<int>(sizeof(timeout_ms))) == 0;
#else
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        return ::setsockopt(sock, SOL_SOCKET, optname, &tv, sizeof(tv)) == 0;
#endif
    }

    void configure_connected_stream_socket(socket_t sock, int idle_timeout_ms)
    {
        if (sock == invalid_socket) {
            return;
        }

        (void)set_socket_timeout(sock, idle_timeout_ms, SO_RCVTIMEO);
        (void)set_socket_timeout(sock, idle_timeout_ms, SO_SNDTIMEO);

        int yes = 1;
        (void)::setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE,
#ifdef _WIN32
                           reinterpret_cast<const char *>(&yes),
#else
                           &yes,
#endif
                           static_cast<socklen_t>(sizeof(yes)));

#ifdef TCP_NODELAY
        (void)::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
#ifdef _WIN32
                           reinterpret_cast<const char *>(&yes),
#else
                           &yes,
#endif
                           static_cast<socklen_t>(sizeof(yes)));
#endif
    }

    bool wait_for_connect_ready(socket_t sock, int timeout_ms)
    {
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(sock, &writefds);

        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int ready = ::select(static_cast<int>(sock + 1), nullptr, &writefds, nullptr, &tv);
        if (ready <= 0) {
            return false;
        }

        int err = 0;
        socklen_t err_len = static_cast<socklen_t>(sizeof(err));
        if (::getsockopt(sock, SOL_SOCKET, SO_ERROR,
#ifdef _WIN32
                         reinterpret_cast<char *>(&err),
#else
                         &err,
#endif
                         &err_len) != 0) {
            return false;
        }
        return err == 0;
    }

    SocketHandle connect_stream_socket(const std::string &host, uint16_t port, int connect_timeout_ms, int idle_timeout_ms)
    {
        auto sock = std::make_unique<yuan::net::Socket>(host, static_cast<int>(port));
        if (!sock->valid()) {
            return nullptr;
        }
        sock->set_none_block(true);
        if (!sock->connect()) {
            return nullptr;
        }
        if (!wait_for_connect_ready(static_cast<socket_t>(sock->get_fd()), connect_timeout_ms)) {
            return nullptr;
        }
        sock->set_none_block(false);
        sock->set_keep_alive(true);
        sock->set_no_delay(true);
        configure_connected_stream_socket(static_cast<socket_t>(sock->get_fd()), idle_timeout_ms);
        return sock;
    }

    SocketHandle create_tcp_listener(const std::string &host, uint16_t port, bool non_blocking)
    {
        auto sock = std::make_unique<yuan::net::Socket>(host, static_cast<int>(port));
        if (!sock->valid()) {
            return nullptr;
        }

        (void)sock->set_reuse(true);
        sock->set_none_block(non_blocking);
        if (!sock->bind() || !sock->listen()) {
            return nullptr;
        }
        return sock;
    }

    SocketHandle create_udp_bound_socket(const std::string &host, uint16_t port)
    {
        auto sock = std::make_unique<yuan::net::Socket>(host, static_cast<int>(port), true);
        if (!sock->valid()) {
            return nullptr;
        }

        (void)sock->set_reuse(true);
        if (!sock->bind()) {
            return nullptr;
        }
        return sock;
    }

    std::string sockaddr_to_string(const sockaddr_storage &addr)
    {
        char host[INET6_ADDRSTRLEN] = { 0 };
        uint16_t port = 0;

        if (addr.ss_family == AF_INET) {
            const auto *sin = reinterpret_cast<const sockaddr_in *>(&addr);
            port = ntohs(sin->sin_port);
            if (!::inet_ntop(AF_INET, &sin->sin_addr, host, sizeof(host))) {
                return "unknown";
            }
        } else if (addr.ss_family == AF_INET6) {
            const auto *sin6 = reinterpret_cast<const sockaddr_in6 *>(&addr);
            port = ntohs(sin6->sin6_port);
            if (!::inet_ntop(AF_INET6, &sin6->sin6_addr, host, sizeof(host))) {
                return "unknown";
            }
        } else {
            return "unknown";
        }

        std::ostringstream oss;
        if (addr.ss_family == AF_INET6) {
            oss << '[' << host << "]:" << port;
        } else {
            oss << host << ':' << port;
        }
        return oss.str();
    }

    bool write_all(socket_t sock, const uint8_t *data, std::size_t len)
    {
        std::size_t offset = 0;
        while (offset < len) {
            const int chunk = static_cast<int>(len - offset);
#ifdef _WIN32
            const int sent = ::send(sock, reinterpret_cast<const char *>(data + offset), chunk, 0);
#else
            const int sent = ::send(sock, data + offset, chunk, 0);
#endif
            if (sent <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(sent);
        }
        return true;
    }

    bool read_exact(socket_t sock, uint8_t *data, std::size_t len)
    {
        std::size_t offset = 0;
        while (offset < len) {
#ifdef _WIN32
            const int received = ::recv(sock, reinterpret_cast<char *>(data + offset), static_cast<int>(len - offset), 0);
#else
            const int received = ::recv(sock, data + offset, static_cast<int>(len - offset), 0);
#endif
            if (received <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(received);
        }
        return true;
    }

    bool read_http_request(socket_t sock, std::string &request)
    {
        request.clear();
        std::vector<char> buffer(4096);
        while (request.size() < 64 * 1024) {
            const int received = ::recv(sock, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received <= 0) {
                return false;
            }
            request.append(buffer.data(), buffer.data() + received);
            if (request.find("\r\n\r\n") != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    std::string to_lower(std::string value)
    {
        for (char &ch : value) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return value;
    }

    std::string trim(std::string value)
    {
        std::size_t begin = 0;
        while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
            ++begin;
        }
        std::size_t end = value.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
            --end;
        }
        return value.substr(begin, end - begin);
    }

    std::unordered_map<std::string, std::string> parse_header_map(const std::string &request)
    {
        std::unordered_map<std::string, std::string> headers;
        const auto header_end = request.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return headers;
        }

        std::size_t line_start = request.find("\r\n");
        if (line_start == std::string::npos) {
            return headers;
        }
        line_start += 2;

        while (line_start < header_end) {
            const auto line_end = request.find("\r\n", line_start);
            if (line_end == std::string::npos || line_end > header_end) {
                break;
            }
            const auto colon = request.find(':', line_start);
            if (colon != std::string::npos && colon < line_end) {
                auto name = to_lower(trim(request.substr(line_start, colon - line_start)));
                auto value = trim(request.substr(colon + 1, line_end - colon - 1));
                if (!name.empty()) {
                    headers[name] = std::move(value);
                }
            }
            line_start = line_end + 2;
        }

        return headers;
    }

    bool verify_basic_proxy_auth(const std::unordered_map<std::string, std::string> &headers,
                                 const HttpProxySettings &settings)
    {
        if (!settings.require_basic_auth) {
            return true;
        }

        const auto it = headers.find("proxy-authorization");
        if (it == headers.end()) {
            return false;
        }
        const std::string &value = it->second;
        if (value.size() < 6 || to_lower(value.substr(0, 6)) != "basic ") {
            return false;
        }

        const std::string decoded = yuan::base::util::base64_decode(value.substr(6));
        const auto sep = decoded.find(':');
        if (sep == std::string::npos) {
            return false;
        }
        return decoded.substr(0, sep) == settings.auth_user &&
               decoded.substr(sep + 1) == settings.auth_password;
    }

    bool write_http_text(socket_t sock, int status_code, const std::string &reason, const std::string &body = {})
    {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << status_code << ' ' << reason << "\r\n"
            << "Proxy-Agent: proxy_tool\r\n"
            << "Connection: close\r\n";
        if (!body.empty()) {
            oss << "Content-Type: text/plain; charset=utf-8\r\n"
                << "Content-Length: " << body.size() << "\r\n";
        } else {
            oss << "Content-Length: 0\r\n";
        }
        oss << "\r\n";
        const std::string header = oss.str();
        if (!write_all(sock, reinterpret_cast<const uint8_t *>(header.data()), header.size())) {
            return false;
        }
        if (!body.empty()) {
            return write_all(sock, reinterpret_cast<const uint8_t *>(body.data()), body.size());
        }
        return true;
    }

    struct ConnectTarget
    {
        std::string host;
        uint16_t port = 443;
    };

    bool parse_connect_target(const std::string &target, ConnectTarget &out)
    {
        if (target.empty()) {
            return false;
        }

        if (target.front() == '[') {
            const auto close = target.find(']');
            if (close == std::string::npos || close + 2 >= target.size() || target[close + 1] != ':') {
                return false;
            }
            out.host = target.substr(1, close - 1);
            out.port = static_cast<uint16_t>(std::stoi(target.substr(close + 2)));
            return true;
        }

        const auto colon = target.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= target.size()) {
            return false;
        }
        if (target.find(':') != colon) {
            return false;
        }

        out.host = target.substr(0, colon);
        out.port = static_cast<uint16_t>(std::stoi(target.substr(colon + 1)));
        return true;
    }

    bool relay_bidirectional(socket_t left, socket_t right, const std::string &tag, int idle_timeout_ms)
    {
        std::vector<uint8_t> buffer(8192);
        std::size_t left_to_right = 0;
        std::size_t right_to_left = 0;

        while (true) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(left, &readfds);
            FD_SET(right, &readfds);

            const socket_t maxfd = left > right ? left : right;
            timeval tv{};
            tv.tv_sec = idle_timeout_ms / 1000;
            tv.tv_usec = (idle_timeout_ms % 1000) * 1000;
            const int ready = ::select(static_cast<int>(maxfd + 1), &readfds, nullptr, nullptr, &tv);
            if (ready <= 0) {
                if (ready == 0) {
                    LOG_WARN("[http-proxy] {} idle timeout reached", tag);
                }
                break;
            }

            if (FD_ISSET(left, &readfds)) {
                const int received = ::recv(left, reinterpret_cast<char *>(buffer.data()), static_cast<int>(buffer.size()), 0);
                if (received <= 0 || !write_all(right, buffer.data(), static_cast<std::size_t>(received))) {
                    LOG_INFO("[http-proxy] {} relay left->right closed, transferred={} bytes", tag, left_to_right);
                    break;
                }
                left_to_right += static_cast<std::size_t>(received);
            }

            if (FD_ISSET(right, &readfds)) {
                const int received = ::recv(right, reinterpret_cast<char *>(buffer.data()), static_cast<int>(buffer.size()), 0);
                if (received <= 0 || !write_all(left, buffer.data(), static_cast<std::size_t>(received))) {
                    LOG_INFO("[http-proxy] {} relay right->left closed, transferred={} bytes", tag, right_to_left);
                    break;
                }
                right_to_left += static_cast<std::size_t>(received);
            }
        }

        shutdown_socket(left);
        shutdown_socket(right);
        LOG_INFO("[http-proxy] {} tunnel shutdown, left->right={} bytes, right->left={} bytes",
                 tag, left_to_right, right_to_left);
        return true;
    }

    void handle_http_proxy_client(socket_t client, const sockaddr_storage &peer)
    {
        static const HttpProxySettings settings = load_http_proxy_settings();
        SessionGuard guard(g_active_http_sessions);
        const std::string peer_text = sockaddr_to_string(peer);
        LOG_INFO("[http-proxy] client connected from {}", peer_text);
        configure_connected_stream_socket(client, settings.header_timeout_ms);

        std::string request;
        if (!read_http_request(client, request)) {
            LOG_WARN("[http-proxy] {} failed to read request: {}", peer_text, socket_error_message());
            close_socket(client);
            return;
        }

        const auto line_end = request.find("\r\n");
        const auto header_end = request.find("\r\n\r\n");
        if (line_end == std::string::npos) {
            LOG_WARN("[http-proxy] {} malformed request line", peer_text);
            (void)write_http_text(client, 400, "Bad Request");
            close_socket(client);
            return;
        }
        const auto headers = parse_header_map(request);
        if (!verify_basic_proxy_auth(headers, settings)) {
            const std::string response =
                "HTTP/1.1 407 Proxy Authentication Required\r\n"
                "Proxy-Agent: proxy_tool\r\n"
                "Proxy-Authenticate: Basic realm=\"yuan-proxy\"\r\n"
                "Connection: close\r\n"
                "Content-Length: 0\r\n\r\n";
            (void)write_all(client, reinterpret_cast<const uint8_t *>(response.data()), response.size());
            close_socket(client);
            return;
        }

        configure_connected_stream_socket(client, settings.idle_timeout_ms);

        std::istringstream iss(request.substr(0, line_end));
        std::string method;
        std::string target;
        std::string version;
        iss >> method >> target >> version;
        if (method.empty() || target.empty()) {
            LOG_WARN("[http-proxy] {} incomplete request line: {}", peer_text, request.substr(0, line_end));
            (void)write_http_text(client, 400, "Bad Request");
            close_socket(client);
            return;
        }

        LOG_INFO("[http-proxy] {} request: {} {} {}", peer_text, method, target, version);

        if (method != "CONNECT") {
            LOG_WARN("[http-proxy] {} rejected method {}", peer_text, method);
            (void)write_http_text(client, 405, "Method Not Allowed", "proxy_tool only supports CONNECT");
            close_socket(client);
            return;
        }

        ConnectTarget connect_target;
        if (!parse_connect_target(target, connect_target)) {
            LOG_WARN("[http-proxy] {} invalid CONNECT target {}", peer_text, target);
            (void)write_http_text(client, 400, "Bad Request", "invalid CONNECT target");
            close_socket(client);
            return;
        }

        LOG_INFO("[http-proxy] {} connecting upstream {}:{}", peer_text, connect_target.host, connect_target.port);

        std::string leftover;
        if (header_end != std::string::npos) {
            leftover = request.substr(header_end + 4);
        }

        auto upstream = connect_stream_socket(connect_target.host, connect_target.port,
                                              settings.connect_timeout_ms, settings.idle_timeout_ms);
        if (!upstream) {
            LOG_ERROR("[http-proxy] {} failed to connect upstream {}:{} : {}",
                      peer_text, connect_target.host, connect_target.port, socket_error_message());
            (void)write_http_text(client, 502, "Bad Gateway", "failed to connect upstream");
            close_socket(client);
            return;
        }

        LOG_INFO("[http-proxy] {} upstream connected {}:{}", peer_text, connect_target.host, connect_target.port);

        const std::string established =
            "HTTP/1.1 200 Connection Established\r\n"
            "Proxy-Agent: proxy_tool\r\n"
            "\r\n";
        if (!write_all(client, reinterpret_cast<const uint8_t *>(established.data()), established.size())) {
            LOG_ERROR("[http-proxy] {} failed to write CONNECT response", peer_text);
            close_socket(client);
            return;
        }

        if (!leftover.empty() &&
            !write_all(static_cast<socket_t>(upstream->get_fd()),
                       reinterpret_cast<const uint8_t *>(leftover.data()), leftover.size())) {
            LOG_ERROR("[http-proxy] {} failed to forward CONNECT leftover bytes", peer_text);
            close_socket(client);
            return;
        }

        LOG_INFO("[http-proxy] {} tunnel established {}:{}", peer_text, connect_target.host, connect_target.port);

        (void)relay_bidirectional(client, static_cast<socket_t>(upstream->get_fd()),
                                  peer_text + " -> " + connect_target.host + ':' + std::to_string(connect_target.port),
                                  settings.idle_timeout_ms);

        close_socket(client);
        LOG_INFO("[http-proxy] {} connection closed", peer_text);
    }

    bool set_recv_timeout(socket_t sock, int timeout_ms)
    {
#ifdef _WIN32
        return ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                            reinterpret_cast<const char *>(&timeout_ms),
                            static_cast<int>(sizeof(timeout_ms))) == 0;
#else
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        return ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
    }

    bool start_proxy_server(uint16_t port)
    {
        yuan::net::socks5::Socks5ServerConfig config;
        config.enable_auth = false;
        config.enable_connect = true;
        config.enable_udp_associate = true;

        yuan::net::socks5::Socks5Server server(config);
        if (!server.init(port)) {
            LOG_ERROR("failed to bind socks5 server on port {}", port);
            return false;
        }

        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);

        std::thread server_thread([&server]() {
            server.serve();
        });

        LOG_INFO("SOCKS5 server listening on 127.0.0.1:{}", port);
        LOG_INFO("SOCKS5 features: CONNECT=on, UDP_ASSOCIATE=on, AUTH=off");
        LOG_INFO("press Ctrl+C to stop");

        while (!g_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        server.stop();
        server_thread.join();
        LOG_INFO("SOCKS5 server stopped");
        return true;
    }

    bool start_http_proxy(uint16_t port)
    {
        const HttpProxySettings settings = load_http_proxy_settings();
        auto listen_socket = create_tcp_listener("0.0.0.0", port, true);
        if (!listen_socket) {
            LOG_ERROR("failed to bind http proxy on port {}", port);
            return false;
        }

        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);

        LOG_INFO("HTTP proxy listening on 0.0.0.0:{}", port);
        LOG_INFO("proxy supports CONNECT tunneling for HTTPS traffic");
        LOG_INFO("http proxy limits: max_active={}, header_timeout_ms={}, idle_timeout_ms={}, connect_timeout_ms={}, basic_auth={}",
                 settings.max_active_sessions, settings.header_timeout_ms, settings.idle_timeout_ms,
                 settings.connect_timeout_ms, settings.require_basic_auth ? "on" : "off");
        LOG_INFO("press Ctrl+C to stop");

        while (!g_stop.load(std::memory_order_relaxed)) {
            sockaddr_storage peer{};
            const socket_t client = static_cast<socket_t>(listen_socket->accept(peer));
#ifdef _WIN32
            if (client == invalid_socket) {
                const int err = ::WSAGetLastError();
                if (err == WSAEWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                LOG_WARN("[http-proxy] accept failed: WSA error {}", err);
                continue;
            }
#else
            if (client == invalid_socket) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                LOG_WARN("[http-proxy] accept failed: {}", socket_error_message());
                continue;
            }
#endif

            const std::string peer_text = sockaddr_to_string(peer);
            if (g_active_http_sessions.load(std::memory_order_relaxed) >= settings.max_active_sessions) {
                LOG_WARN("[http-proxy] reject {} due to active session limit", peer_text);
                (void)write_http_text(client, 503, "Service Unavailable", "proxy busy");
                close_socket(client);
                continue;
            }
            LOG_INFO("[http-proxy] accepted client {}", peer_text);

            std::thread([client, peer]() {
                handle_http_proxy_client(client, peer);
            }).detach();
        }

        LOG_INFO("[http-proxy] listener stopped");
        return true;
    }

    bool start_udp_echo(uint16_t port)
    {
        auto socket = create_udp_bound_socket("127.0.0.1", port);
        if (!socket) {
            LOG_ERROR("failed to start udp echo on 127.0.0.1:{}", port);
            return false;
        }

        const socket_t sock = static_cast<socket_t>(socket->get_fd());
        LOG_INFO("UDP echo server listening on 127.0.0.1:{}", port);
        set_recv_timeout(sock, 1000);

        std::vector<uint8_t> buffer(4096);
        sockaddr_storage peer{};
        while (!g_stop.load(std::memory_order_relaxed)) {
            socklen_t peer_len = static_cast<socklen_t>(sizeof(peer));
            const int received = ::recvfrom(sock, reinterpret_cast<char *>(buffer.data()), static_cast<int>(buffer.size()), 0,
                                            reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if (received <= 0) {
                continue;
            }

#ifdef _WIN32
            (void)::sendto(sock, reinterpret_cast<const char *>(buffer.data()), received, 0,
                           reinterpret_cast<sockaddr *>(&peer), peer_len);
#else
            (void)::sendto(sock, buffer.data(), received, 0,
                           reinterpret_cast<sockaddr *>(&peer), peer_len);
#endif
        }

        return true;
    }

    int run_udp_probe(const std::string &proxy_host, uint16_t proxy_port,
                      const std::string &target_host, uint16_t target_port,
                      const std::string &payload_text)
    {
        auto tcp_socket = connect_stream_socket(proxy_host, proxy_port, 5000, 10000);
        if (!tcp_socket) {
            LOG_ERROR("failed to connect to proxy {}:{}", proxy_host, proxy_port);
            return 1;
        }
        const socket_t tcp = static_cast<socket_t>(tcp_socket->get_fd());

        uint8_t greeting[] = { 0x05, 0x01, 0x00 };
        if (!write_all(tcp, greeting, sizeof(greeting))) {
            LOG_ERROR("failed to send greeting");
            return 1;
        }

        uint8_t greet_reply[2] = { 0 };
        if (!read_exact(tcp, greet_reply, sizeof(greet_reply))) {
            LOG_ERROR("failed to read greeting reply");
            return 1;
        }
        if (greet_reply[0] != 0x05 || greet_reply[1] != 0x00) {
            LOG_ERROR("proxy did not accept no-auth greeting");
            return 1;
        }

        uint8_t udp_assoc[] = {
            0x05, 0x03, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00
        };
        if (!write_all(tcp, udp_assoc, sizeof(udp_assoc))) {
            LOG_ERROR("failed to send udp associate request");
            return 1;
        }

        uint8_t reply[10] = { 0 };
        if (!read_exact(tcp, reply, sizeof(reply))) {
            LOG_ERROR("failed to read udp associate reply");
            return 1;
        }
        if (reply[1] != 0x00) {
            LOG_ERROR("udp associate rejected with code {}", static_cast<int>(reply[1]));
            return 1;
        }

        const uint16_t relay_port = static_cast<uint16_t>((static_cast<uint16_t>(reply[8]) << 8) | reply[9]);
        if (relay_port == 0) {
            LOG_ERROR("proxy returned relay port 0");
            return 1;
        }

        auto udp_socket = std::make_unique<yuan::net::Socket>("0.0.0.0", 0, true);
        if (!udp_socket || !udp_socket->valid()) {
            LOG_ERROR("failed to create udp socket");
            return 1;
        }
        const socket_t udp = static_cast<socket_t>(udp_socket->get_fd());
        set_recv_timeout(udp, 4000);

        sockaddr_in relay{};
        relay.sin_family = AF_INET;
        relay.sin_port = htons(relay_port);
        relay.sin_addr.s_addr = inet_addr(proxy_host.c_str());

        std::vector<uint8_t> packet;
        packet.reserve(10 + payload_text.size());
        packet.push_back(0x00);
        packet.push_back(0x00);
        packet.push_back(0x00);
        packet.push_back(0x01);

        in_addr target_addr{};
        if (::inet_pton(AF_INET, target_host.c_str(), &target_addr) != 1) {
            LOG_ERROR("only ipv4 target is supported in this probe helper");
            return 1;
        }

        const auto *target_bytes = reinterpret_cast<const uint8_t *>(&target_addr.s_addr);
        packet.insert(packet.end(), target_bytes, target_bytes + 4);

        const uint16_t net_port = htons(target_port);
        const auto *port_bytes = reinterpret_cast<const uint8_t *>(&net_port);
        packet.insert(packet.end(), port_bytes, port_bytes + 2);
        packet.insert(packet.end(), payload_text.begin(), payload_text.end());

#ifdef _WIN32
        if (::sendto(udp, reinterpret_cast<const char *>(packet.data()), static_cast<int>(packet.size()), 0,
                     reinterpret_cast<sockaddr *>(&relay), sizeof(relay)) < 0) {
#else
        if (::sendto(udp, packet.data(), static_cast<int>(packet.size()), 0,
                     reinterpret_cast<sockaddr *>(&relay), sizeof(relay)) < 0) {
#endif
            LOG_ERROR("failed to send udp datagram through proxy");
            return 1;
        }

        std::vector<uint8_t> response(4096);
        sockaddr_storage from{};
        socklen_t from_len = static_cast<socklen_t>(sizeof(from));
        const int received = ::recvfrom(udp, reinterpret_cast<char *>(response.data()), static_cast<int>(response.size()), 0,
                                        reinterpret_cast<sockaddr *>(&from), &from_len);
        if (received <= 0) {
            LOG_ERROR("did not receive udp response from proxy");
            return 1;
        }

        if (received < 10) {
            LOG_ERROR("udp response too short");
            return 1;
        }

        const std::string echoed_payload(response.begin() + 10, response.begin() + received);
        LOG_INFO("UDP_OK relay_port={} payload={}", relay_port, echoed_payload);
        return 0;
    }

    void print_usage()
    {
        LOG_INFO("proxy_tool commands:\n"
                 "  serve [port]\n"
                 "  http-proxy [port]\n"
                 "  udp-echo [port]\n"
                 "  udp-probe [proxy_host] [proxy_port] [target_host] [target_port] [payload]\n");
    }
} // namespace

int main(int argc, char **argv)
{
    try {
#ifndef _WIN32
        std::signal(SIGPIPE, SIG_IGN);
#endif

        //if (argc < 2) {
        //    print_usage();
        //    return 1;
        //}

        const std::string command = "http-proxy";//argv[1];
        int code = 0;
        if (command == "serve") {
            const uint16_t port = argc >= 3 ? static_cast<uint16_t>(std::stoi(argv[2])) : 1080;
            if (!start_proxy_server(port)) {
                code = 1;
            }
        } else if (command == "http-proxy") {
            const uint16_t port = argc >= 3 ? static_cast<uint16_t>(std::stoi(argv[2])) : 3128;
            if (!start_http_proxy(port)) {
                code = 1;
            }
        } else if (command == "udp-echo") {
            const uint16_t port = argc >= 3 ? static_cast<uint16_t>(std::stoi(argv[2])) : 19090;
            if (!start_udp_echo(port)) {
                code = 1;
            }
        } else if (command == "udp-probe") {
            if (argc < 7) {
                print_usage();
                return 1;
            }
            const std::string proxy_host = argv[2];
            const uint16_t proxy_port = static_cast<uint16_t>(std::stoi(argv[3]));
            const std::string target_host = argv[4];
            const uint16_t target_port = static_cast<uint16_t>(std::stoi(argv[5]));
            std::string payload = argv[6];
            for (int i = 7; i < argc; ++i) {
                payload.push_back(' ');
                payload += argv[i];
            }
            code = run_udp_probe(proxy_host, proxy_port, target_host, target_port, payload);
        } else {
            print_usage();
            code = 1;
        }

        return code;
    } catch (const std::exception &ex) {
        LOG_ERROR("{}", ex.what());
        return 1;
    }
}
