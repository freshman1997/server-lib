#include "socks5.h"

#include <atomic>
#include <exception>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t invalid_socket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t invalid_socket = -1;
#endif

namespace
{
    std::atomic_bool g_stop{false};

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

    bool resolve_and_connect(socket_t &sock, const std::string &host, uint16_t port)
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo *result = nullptr;
        const std::string port_str = std::to_string(port);
        if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0 || !result) {
            return false;
        }

        for (auto *ai = result; ai; ai = ai->ai_next) {
            sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (sock == invalid_socket) {
                continue;
            }

            if (::connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
                ::freeaddrinfo(result);
                return true;
            }

            close_socket(sock);
            sock = invalid_socket;
        }

        ::freeaddrinfo(result);
        return false;
    }

    bool set_reuse_addr(socket_t sock)
    {
        int yes = 1;
        return ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                            reinterpret_cast<const char *>(&yes),
                            static_cast<int>(sizeof(yes))) == 0;
    }

    bool set_nonblocking(socket_t sock)
    {
#ifdef _WIN32
        u_long mode = 1;
        return ::ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
        const int flags = ::fcntl(sock, F_GETFL, 0);
        if (flags < 0) {
            return false;
        }
        return ::fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }

    bool bind_listen_socket(socket_t &sock, const std::string &host, uint16_t port, int backlog = 64)
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        addrinfo *result = nullptr;
        const std::string port_str = std::to_string(port);
        const char *bind_host = host.empty() ? nullptr : host.c_str();
        if (::getaddrinfo(bind_host, port_str.c_str(), &hints, &result) != 0 || !result) {
            return false;
        }

        for (auto *ai = result; ai; ai = ai->ai_next) {
            sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (sock == invalid_socket) {
                continue;
            }

            (void)set_reuse_addr(sock);

            if (::bind(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0 &&
                ::listen(sock, backlog) == 0) {
                ::freeaddrinfo(result);
                return true;
            }

            close_socket(sock);
            sock = invalid_socket;
        }

        ::freeaddrinfo(result);
        return false;
    }

    bool read_http_request(socket_t sock, std::string &request)
    {
        request.clear();
        std::vector<char> buffer(4096);
        while (request.size() < 64 * 1024) {
#ifdef _WIN32
            const int received = ::recv(sock, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
            const int received = ::recv(sock, buffer.data(), static_cast<int>(buffer.size()), 0);
#endif
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

        if (!target.empty() && target.front() == '[') {
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

    bool relay_bidirectional(socket_t left, socket_t right)
    {
        std::vector<uint8_t> buffer(8192);
        while (true) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(left, &readfds);
            FD_SET(right, &readfds);

            const socket_t maxfd = left > right ? left : right;
            const int ready = ::select(static_cast<int>(maxfd + 1), &readfds, nullptr, nullptr, nullptr);
            if (ready <= 0) {
                break;
            }

            if (FD_ISSET(left, &readfds)) {
#ifdef _WIN32
                const int received = ::recv(left, reinterpret_cast<char *>(buffer.data()), static_cast<int>(buffer.size()), 0);
#else
                const int received = ::recv(left, buffer.data(), static_cast<int>(buffer.size()), 0);
#endif
                if (received <= 0 || !write_all(right, buffer.data(), static_cast<std::size_t>(received))) {
                    break;
                }
            }

            if (FD_ISSET(right, &readfds)) {
#ifdef _WIN32
                const int received = ::recv(right, reinterpret_cast<char *>(buffer.data()), static_cast<int>(buffer.size()), 0);
#else
                const int received = ::recv(right, buffer.data(), static_cast<int>(buffer.size()), 0);
#endif
                if (received <= 0 || !write_all(left, buffer.data(), static_cast<std::size_t>(received))) {
                    break;
                }
            }
        }

        shutdown_socket(left);
        shutdown_socket(right);
        return true;
    }

    void handle_http_proxy_client(socket_t client)
    {
        std::string request;
        if (!read_http_request(client, request)) {
            close_socket(client);
            return;
        }

        const auto line_end = request.find("\r\n");
        if (line_end == std::string::npos) {
            (void)write_http_text(client, 400, "Bad Request");
            close_socket(client);
            return;
        }

        std::istringstream iss(request.substr(0, line_end));
        std::string method;
        std::string target;
        std::string version;
        iss >> method >> target >> version;
        if (method.empty() || target.empty()) {
            (void)write_http_text(client, 400, "Bad Request");
            close_socket(client);
            return;
        }

        if (method != "CONNECT") {
            (void)write_http_text(client, 405, "Method Not Allowed", "proxy_tool only supports CONNECT");
            close_socket(client);
            return;
        }

        ConnectTarget connect_target;
        if (!parse_connect_target(target, connect_target)) {
            (void)write_http_text(client, 400, "Bad Request", "invalid CONNECT target");
            close_socket(client);
            return;
        }

        socket_t upstream = invalid_socket;
        if (!resolve_and_connect(upstream, connect_target.host, connect_target.port)) {
            (void)write_http_text(client, 502, "Bad Gateway", "failed to connect upstream");
            close_socket(client);
            return;
        }

        const std::string established =
            "HTTP/1.1 200 Connection Established\r\n"
            "Proxy-Agent: proxy_tool\r\n"
            "\r\n";
        if (!write_all(client, reinterpret_cast<const uint8_t *>(established.data()), established.size())) {
            close_socket(upstream);
            close_socket(client);
            return;
        }

        (void)relay_bidirectional(client, upstream);

        close_socket(upstream);
        close_socket(client);
    }

    bool bind_udp_socket(socket_t &sock, const std::string &host, uint16_t port)
    {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_PASSIVE;

        addrinfo *result = nullptr;
        const std::string port_str = std::to_string(port);
        if (::getaddrinfo(host.empty() ? nullptr : host.c_str(), port_str.c_str(), &hints, &result) != 0 || !result) {
            return false;
        }

        sock = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (sock == invalid_socket) {
            ::freeaddrinfo(result);
            return false;
        }

        const bool ok = ::bind(sock, result->ai_addr, static_cast<int>(result->ai_addrlen)) == 0;
        ::freeaddrinfo(result);
        if (!ok) {
            close_socket(sock);
            sock = invalid_socket;
        }
        return ok;
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
            std::cerr << "failed to bind socks5 server on port " << port << "\n";
            return false;
        }

        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);

        std::thread server_thread([&server]() {
            server.serve();
        });

        std::cout << "SOCKS5 server listening on 127.0.0.1:" << port << "\n";
        std::cout << "press Ctrl+C to stop\n";

        while (!g_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        server.stop();
        server_thread.join();
        return true;
    }

    bool start_http_proxy(uint16_t port)
    {
        socket_t listen_sock = invalid_socket;
        if (!bind_listen_socket(listen_sock, "0.0.0.0", port)) {
            std::cerr << "failed to bind http proxy on port " << port << "\n";
            return false;
        }

        if (!set_nonblocking(listen_sock)) {
            std::cerr << "failed to switch http proxy socket to non-blocking mode\n";
            close_socket(listen_sock);
            return false;
        }

        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);

        std::cout << "HTTP proxy listening on 0.0.0.0:" << port << "\n";
        std::cout << "proxy supports CONNECT tunneling for HTTPS traffic\n";
        std::cout << "press Ctrl+C to stop\n";

        while (!g_stop.load(std::memory_order_relaxed)) {
            sockaddr_storage peer{};
            socklen_t peer_len = static_cast<socklen_t>(sizeof(peer));
#ifdef _WIN32
            const socket_t client = ::accept(listen_sock, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if (client == invalid_socket) {
                const int err = ::WSAGetLastError();
                if (err == WSAEWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                continue;
            }
#else
            const socket_t client = ::accept(listen_sock, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if (client == invalid_socket) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                continue;
            }
#endif

            std::thread([client]() {
                handle_http_proxy_client(client);
            }).detach();
        }

        close_socket(listen_sock);
        return true;
    }

    bool start_udp_echo(uint16_t port)
    {
        socket_t sock = invalid_socket;
        if (!bind_udp_socket(sock, "127.0.0.1", port)) {
            std::cerr << "failed to start udp echo on 127.0.0.1:" << port << "\n";
            return false;
        }

        std::cout << "UDP echo server listening on 127.0.0.1:" << port << "\n";
        set_recv_timeout(sock, 1000);

        std::vector<uint8_t> buffer(4096);
        sockaddr_storage peer{};
        while (!g_stop.load(std::memory_order_relaxed)) {
            socklen_t peer_len = static_cast<socklen_t>(sizeof(peer));
#ifdef _WIN32
            const int received = ::recvfrom(sock, reinterpret_cast<char *>(buffer.data()), static_cast<int>(buffer.size()), 0,
                                            reinterpret_cast<sockaddr *>(&peer), &peer_len);
#else
            const int received = ::recvfrom(sock, buffer.data(), static_cast<int>(buffer.size()), 0,
                                            reinterpret_cast<sockaddr *>(&peer), &peer_len);
#endif
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

        close_socket(sock);
        return true;
    }

    int run_udp_probe(const std::string &proxy_host, uint16_t proxy_port,
                      const std::string &target_host, uint16_t target_port,
                      const std::string &payload_text)
    {
        socket_t tcp = invalid_socket;
        if (!resolve_and_connect(tcp, proxy_host, proxy_port)) {
            std::cerr << "failed to connect to proxy " << proxy_host << ":" << proxy_port << "\n";
            return 1;
        }

        uint8_t greeting[] = { 0x05, 0x01, 0x00 };
        if (!write_all(tcp, greeting, sizeof(greeting))) {
            std::cerr << "failed to send greeting\n";
            close_socket(tcp);
            return 1;
        }

        uint8_t greet_reply[2] = {0};
        if (!read_exact(tcp, greet_reply, sizeof(greet_reply))) {
            std::cerr << "failed to read greeting reply\n";
            close_socket(tcp);
            return 1;
        }
        if (greet_reply[0] != 0x05 || greet_reply[1] != 0x00) {
            std::cerr << "proxy did not accept no-auth greeting\n";
            close_socket(tcp);
            return 1;
        }

        uint8_t udp_assoc[] = {
            0x05, 0x03, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00
        };
        if (!write_all(tcp, udp_assoc, sizeof(udp_assoc))) {
            std::cerr << "failed to send udp associate request\n";
            close_socket(tcp);
            return 1;
        }

        uint8_t reply[10] = {0};
        if (!read_exact(tcp, reply, sizeof(reply))) {
            std::cerr << "failed to read udp associate reply\n";
            close_socket(tcp);
            return 1;
        }
        if (reply[1] != 0x00) {
            std::cerr << "udp associate rejected with code " << static_cast<int>(reply[1]) << "\n";
            close_socket(tcp);
            return 1;
        }

        const uint16_t relay_port = static_cast<uint16_t>((static_cast<uint16_t>(reply[8]) << 8) | reply[9]);
        if (relay_port == 0) {
            std::cerr << "proxy returned relay port 0\n";
            close_socket(tcp);
            return 1;
        }

        socket_t udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udp == invalid_socket) {
            std::cerr << "failed to create udp socket\n";
            close_socket(tcp);
            return 1;
        }
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
            std::cerr << "only ipv4 target is supported in this probe helper\n";
            close_socket(udp);
            close_socket(tcp);
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
            std::cerr << "failed to send udp datagram through proxy\n";
            close_socket(udp);
            close_socket(tcp);
            return 1;
        }

        std::vector<uint8_t> response(4096);
        sockaddr_storage from{};
        socklen_t from_len = static_cast<socklen_t>(sizeof(from));
#ifdef _WIN32
        const int received = ::recvfrom(udp, reinterpret_cast<char *>(response.data()), static_cast<int>(response.size()), 0,
                                        reinterpret_cast<sockaddr *>(&from), &from_len);
#else
        const int received = ::recvfrom(udp, response.data(), static_cast<int>(response.size()), 0,
                                        reinterpret_cast<sockaddr *>(&from), &from_len);
#endif
        if (received <= 0) {
            std::cerr << "did not receive udp response from proxy\n";
            close_socket(udp);
            close_socket(tcp);
            return 1;
        }

        if (received < 10) {
            std::cerr << "udp response too short\n";
            close_socket(udp);
            close_socket(tcp);
            return 1;
        }

        const std::string echoed_payload(response.begin() + 10, response.begin() + received);
        std::cout << "UDP_OK relay_port=" << relay_port
                  << " payload=" << echoed_payload << "\n";

        close_socket(udp);
        close_socket(tcp);
        return 0;
    }

    void print_usage()
    {
        std::cout <<
            "proxy_tool commands:\n"
            "  serve [port]\n"
            "  http-proxy [port]\n"
            "  udp-echo [port]\n"
            "  udp-probe [proxy_host] [proxy_port] [target_host] [target_port] [payload]\n";
    }
} // namespace

int main(int argc, char **argv)
{
    try {
#ifndef _WIN32
        std::signal(SIGPIPE, SIG_IGN);
#endif

        if (argc < 2) {
            print_usage();
            return 1;
        }

        const std::string command = argv[1];
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
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
