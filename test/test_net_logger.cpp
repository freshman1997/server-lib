#include "net_logger.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{

void require(bool cond, const std::string& message)
{
    if (!cond) {
        throw std::runtime_error(message);
    }
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

int reserve_free_port()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(fd >= 0, "reserve socket create failed");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    require(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "reserve bind failed");

    sockaddr_in bound{};
#ifdef _WIN32
    int len = sizeof(bound);
    require(::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0, "reserve getsockname failed");
    ::closesocket(fd);
#else
    socklen_t len = sizeof(bound);
    require(::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0, "reserve getsockname failed");
    ::close(fd);
#endif
    return ntohs(bound.sin_port);
}

} // namespace

int main()
{
    using namespace yuan::log;

#ifdef _WIN32
    WSADATA wsa;
    require(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup failed");
#endif

    const int selected_port = reserve_free_port();
    std::atomic<bool> done{false};
    std::string received;

    std::thread server([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));

        const int listen_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(listen_fd >= 0, "socket create failed");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(selected_port));
        require(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind failed");
        require(::listen(listen_fd, 1) == 0, "listen failed");

        sockaddr_in peer{};
#ifdef _WIN32
        int peer_len = sizeof(peer);
#else
        socklen_t peer_len = sizeof(peer);
#endif
        const int conn_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        require(conn_fd >= 0, "accept failed");

        char buffer[4096] = {0};
#ifdef _WIN32
        const int n = ::recv(conn_fd, buffer, sizeof(buffer), 0);
        ::closesocket(conn_fd);
        ::closesocket(listen_fd);
#else
        const int n = static_cast<int>(::recv(conn_fd, buffer, sizeof(buffer), 0));
        ::close(conn_fd);
        ::close(listen_fd);
#endif
        require(n > 0, "recv failed");
        received.assign(buffer, buffer + n);
        done.store(true);
    });

    {
        LogConfig cfg;
        cfg.log_level = Level::trace;
        cfg.net_server_ip = "127.0.0.1";
        cfg.net_server_port = selected_port;
        cfg.net_connect_timeout_ms = 200;
        cfg.net_reconnect_delay_ms = 150;
        cfg.net_max_retries = 20;
        cfg.fmt_pattern = "{levelname}:{message}";

        NetLogger logger(cfg);
        logger.set_name("net");
        require(logger.connect(), "net logger connect initiation failed");

        logger.log_fmt_source(Level::error, __FILE__, __LINE__, __func__, "网络日志 {}", "已送达");
        logger.flush();

        require(wait_until([&]() { return done.load(); }, 8000), "server did not receive log");
        require(received.find("ERROR:网络日志 已送达") != std::string::npos, "received payload mismatch");

        logger.disconnect();
    }

    server.join();

#ifdef _WIN32
    WSACleanup();
#endif

    std::cout << "net_logger test passed\n";
    return 0;
}
