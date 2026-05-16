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

#include <atomic>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
int failures = 0;

bool require(bool condition, const std::string &message)
{
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

void close_socket(socket_t socket)
{
    if (socket == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
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

    const auto port = static_cast<uint16_t>(ntohs(bound.sin_port));
    close_socket(listener);
    return port;
}

bool connect_once(uint16_t port)
{
    socket_t client = ::socket(AF_INET, SOCK_STREAM, 0);
    if (client == kInvalidSocket) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    const bool connected = ::connect(client, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0;
    close_socket(client);
    return connected;
}

socket_t connect_socket_with_retry(uint16_t port)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
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

bool connect_with_retry(uint16_t port)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (connect_once(port)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

bool send_all(socket_t socket, const std::string &text)
{
    std::size_t sent = 0;
    while (sent < text.size()) {
        const auto remaining = static_cast<int>(text.size() - sent);
        const int n = ::send(socket, text.data() + sent, remaining, 0);
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
    out.reserve(expected);
    char buffer[128];
    while (out.size() < expected) {
        const auto remaining = std::min<std::size_t>(sizeof(buffer), expected - out.size());
        const int n = ::recv(socket, buffer, static_cast<int>(remaining), 0);
        if (n <= 0) {
            return false;
        }
        out.append(buffer, static_cast<std::size_t>(n));
    }
    return true;
}

bool wait_for_accept(const std::atomic<int> &accepted)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (accepted.load(std::memory_order_acquire) > 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void run_basic_shutdown_test()
{
    const auto port = reserve_tcp_port();
    require(port != 0, "should reserve a TCP port");

    yuan::net::NetworkRuntime runtime;
    yuan::net::AsyncListenerHost listener;
    yuan::net::ListenOptions options;
    options.reuse_addr = true;
    options.non_block = true;
    options.backlog = 16;

    require(listener.bind("127.0.0.1", port, runtime, options),
            "listener should bind loopback port");

    std::atomic<int> accepted{0};
    listener.set_connection_handler([&](yuan::net::AsyncConnectionContext ctx) -> yuan::coroutine::Task<void> {
        accepted.fetch_add(1, std::memory_order_release);
        ctx.close();
        co_return;
    });

    auto accept_task = listener.run_async();
    accept_task.resume();

    std::thread loop_thread([&runtime]() {
        runtime.run();
    });

    require(connect_with_retry(port), "listener should accept a TCP connection");
    require(wait_for_accept(accepted), "async listener handler should observe accepted connection");

    runtime.dispatch([&listener, &runtime]() {
        listener.close();
        runtime.stop();
    });

    if (loop_thread.joinable()) {
        loop_thread.join();
    }

    require(!listener.is_listening(), "listener should be closed after runtime-dispatched shutdown");
}

void run_coroutine_and_connection_lifecycle_test()
{
    const auto port = reserve_tcp_port();
    require(port != 0, "should reserve a TCP port for lifecycle test");

    constexpr int client_count = 12;
    yuan::net::NetworkRuntime runtime;
    yuan::net::AsyncListenerHost listener;
    yuan::net::ListenOptions options;
    options.reuse_addr = true;
    options.non_block = true;
    options.backlog = 32;

    require(listener.bind("127.0.0.1", port, runtime, options),
            "lifecycle listener should bind loopback port");

    std::atomic<int> started{0};
    std::atomic<int> echoed{0};
    std::atomic<int> finished{0};
    std::atomic<bool> watchdog_fired{false};
    std::mutex weak_mutex;
    std::vector<std::weak_ptr<yuan::net::Connection>> weak_connections;

    listener.set_connection_handler([&](yuan::net::AsyncConnectionContext ctx) -> yuan::coroutine::Task<void> {
        if (auto conn = ctx.connection()) {
            std::lock_guard<std::mutex> lock(weak_mutex);
            weak_connections.push_back(conn);
        }
        started.fetch_add(1, std::memory_order_release);

        const auto read = co_await ctx.read_async(3000);
        if (read.status == yuan::coroutine::IoStatus::success && read.data.readable_bytes() > 0) {
            auto write = co_await ctx.write_async(read.data, 3000);
            if (write.status == yuan::coroutine::IoStatus::success) {
                echoed.fetch_add(1, std::memory_order_release);
            }
        }

        const auto close_status = co_await ctx.close_async();
        require(close_status == yuan::coroutine::IoStatus::success ||
                    close_status == yuan::coroutine::IoStatus::connection_closed,
                "detached handler close_async should complete");

        const int done = finished.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (done == client_count) {
            runtime.stop();
        }
        co_return;
    });

    auto accept_task = listener.run_async();
    accept_task.resume();

    runtime.schedule(7000, [&]() {
        watchdog_fired.store(true, std::memory_order_release);
        runtime.stop();
    });

    std::thread loop_thread([&runtime]() {
        runtime.run();
    });

    std::atomic<int> client_echoes{0};
    std::vector<std::thread> clients;
    clients.reserve(client_count);
    for (int i = 0; i < client_count; ++i) {
        clients.emplace_back([&, i]() {
            const auto socket = connect_socket_with_retry(port);
            if (socket == kInvalidSocket) {
                return;
            }
            const auto payload = "msg-" + std::to_string(i);
            std::string response;
            if (send_all(socket, payload) && recv_exact(socket, response, payload.size()) && response == payload) {
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

    if (loop_thread.joinable()) {
        loop_thread.join();
    }
    listener.close();

    require(!watchdog_fired.load(std::memory_order_acquire), "lifecycle test should finish without watchdog");
    require(started.load(std::memory_order_acquire) == client_count,
            "detached handlers should start for every accepted connection");
    require(echoed.load(std::memory_order_acquire) == client_count,
            "detached handlers should echo every client payload");
    require(finished.load(std::memory_order_acquire) == client_count,
            "detached handlers should finish after close_async");
    require(client_echoes.load(std::memory_order_acquire) == client_count,
            "clients should receive echoed payloads");

    std::lock_guard<std::mutex> lock(weak_mutex);
    const auto leaked = std::count_if(weak_connections.begin(), weak_connections.end(), [](const auto &weak) {
        return !weak.expired();
    });
    require(leaked == 0, "connection objects should be released after handler close and loop drain");
}
} // namespace

int main()
{
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 1;
    }
#endif

    run_basic_shutdown_test();
    run_coroutine_and_connection_lifecycle_test();

#ifdef _WIN32
    WSACleanup();
#endif
    if (failures != 0) {
        std::cerr << "async listener shutdown failed=" << failures << '\n';
        return 1;
    }

    std::cout << "async listener shutdown test passed\n";
    return 0;
}
