#include "net/async/async_listener_host.h"
#include "net/runtime/network_runtime.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    void close_socket(socket_t socket)
    {
        if (socket == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        ::closesocket(socket);
#else
        ::close(socket);
#endif
    }

    std::uint16_t reserve_tcp_port()
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

        const auto port = static_cast<std::uint16_t>(ntohs(bound.sin_port));
        close_socket(listener);
        return port;
    }

    socket_t connect_socket_with_retry(std::uint16_t port)
    {
        for (int attempt = 0; attempt < 80; ++attempt) {
            socket_t client = ::socket(AF_INET, SOCK_STREAM, 0);
            if (client == kInvalidSocket) {
                return kInvalidSocket;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            if (::connect(client, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0) {
                return client;
            }
            close_socket(client);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return kInvalidSocket;
    }

    bool send_all(socket_t socket, const std::string &text)
    {
        std::size_t sent = 0;
        while (sent < text.size()) {
            const int n = ::send(socket,
                                 text.data() + sent,
                                 static_cast<int>(text.size() - sent),
                                 0);
            if (n <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    bool recv_exact(socket_t socket, std::string &out, std::size_t expected)
    {
        out.clear();
        char buffer[128];
        while (out.size() < expected) {
            const auto remaining = (std::min<std::size_t>)(sizeof(buffer), expected - out.size());
            const int n = ::recv(socket, buffer, static_cast<int>(remaining), 0);
            if (n <= 0) {
                return false;
            }
            out.append(buffer, static_cast<std::size_t>(n));
        }
        return true;
    }

    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

    int run_sharded_echo_test()
    {
        const auto port = reserve_tcp_port();
        if (!require(port != 0, "should reserve TCP port")) {
            return 10;
        }

        yuan::net::NetworkRuntime owner_runtime;
        yuan::net::AsyncListenerHost listener;
        yuan::net::ListenOptions options;
        options.reuse_addr = true;
        options.non_block = true;
        options.backlog = 64;
        options.scheduling_mode = yuan::net::ListenSchedulingMode::affinity;
#ifdef _WIN32
        options.shard_count = 1;
#else
        options.shard_count = 2;
#endif

        std::atomic<int> started{0};
        std::atomic<int> echoed{0};
        std::atomic<int> finished{0};
        listener.set_connection_handler([&](yuan::net::AsyncConnectionContext ctx) -> yuan::coroutine::Task<void> {
            started.fetch_add(1, std::memory_order_release);
            const auto read = co_await ctx.read_async(3000);
            if (read.status == yuan::coroutine::IoStatus::success && read.data.readable_bytes() > 0) {
                auto write = co_await ctx.write_async(read.data, 3000);
                if (write.status == yuan::coroutine::IoStatus::success) {
                    auto flush = co_await ctx.flush_async(3000);
                    if (flush.status == yuan::coroutine::IoStatus::success) {
                        echoed.fetch_add(1, std::memory_order_release);
                    }
                }
            }
            (void)co_await ctx.close_async();
            finished.fetch_add(1, std::memory_order_release);
            co_return;
        });

        if (!require(listener.bind("127.0.0.1", port, owner_runtime, options),
                     "sharded listener should bind")) {
            return 11;
        }
        auto task = listener.run_async();
        task.resume();
        if (!require(listener.is_listening(), "sharded listener should be listening after run_async")) {
            return 12;
        }

        constexpr int client_count = 16;
        std::atomic<int> client_echoes{0};
        std::vector<std::thread> clients;
        clients.reserve(client_count);
        for (int i = 0; i < client_count; ++i) {
            clients.emplace_back([&, i]() {
                const auto socket = connect_socket_with_retry(port);
                if (socket == kInvalidSocket) {
                    return;
                }
                const auto payload = "shard-msg-" + std::to_string(i);
                std::string response;
                if (send_all(socket, payload) &&
                    recv_exact(socket, response, payload.size()) &&
                    response == payload) {
                    client_echoes.fetch_add(1, std::memory_order_release);
                }
                close_socket(socket);
            });
        }

        for (auto &client : clients) {
            if (client.joinable()) {
                client.join();
            }
        }

        for (int attempt = 0; attempt < 100; ++attempt) {
            if (finished.load(std::memory_order_acquire) == client_count) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        listener.close();
        owner_runtime.stop();

        if (!require(started.load(std::memory_order_acquire) == client_count,
                     "all sharded handlers should start")) {
            return 13;
        }
        if (!require(echoed.load(std::memory_order_acquire) == client_count,
                     "all sharded handlers should echo")) {
            return 14;
        }
        if (!require(finished.load(std::memory_order_acquire) == client_count,
                     "all sharded handlers should finish")) {
            return 15;
        }
        if (!require(client_echoes.load(std::memory_order_acquire) == client_count,
                     "all clients should receive echo")) {
            return 16;
        }
        if (!require(!listener.is_listening(), "listener should close cleanly")) {
            return 17;
        }

        return 0;
    }
}

int main()
{
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 1;
    }
#endif

    const int result = run_sharded_echo_test();

#ifdef _WIN32
    WSACleanup();
#endif

    if (result != 0) {
        return result;
    }
    std::cout << "sharded async listener test passed\n";
    return 0;
}
