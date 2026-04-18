#include "buffer/byte_buffer.h"
#include "coroutine/io_result.h"
#include "coroutine/runtime.h"
#include "coroutine/sync_wait.h"
#include "coroutine/task.h"
#include "net/async/async_client_session.h"
#include "net/async/async_connection_context.h"
#include "net/async/async_datagram_client.h"
#include "net/async/async_listener_host.h"
#include "net/async/async_request_client.h"
#include "net/runtime/network_runtime.h"
#include "common/winsock_guard.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <WS2tcpip.h>
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
    int g_passed = 0;
    int g_failed = 0;

    void check(bool condition, const std::string &msg)
    {
        if (condition) {
            g_passed++;
        } else {
            std::cerr << "  FAIL: " << msg << "\n";
            g_failed++;
        }
    }

    std::string buffer_to_string(const yuan::buffer::ByteBuffer &buffer)
    {
        const auto span = buffer.readable_span();
        return std::string(span.begin(), span.end());
    }

    uint16_t reserve_tcp_port()
    {
#ifdef _WIN32
        SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET)
            return 0;
#else
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return 0;
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(sock, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
            ::closesocket(sock);
#else
            ::close(sock);
#endif
            return 0;
        }
        sockaddr_in bound{};
#ifdef _WIN32
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        if (::getsockname(sock, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
#ifdef _WIN32
            ::closesocket(sock);
#else
            ::close(sock);
#endif
            return 0;
        }
#ifdef _WIN32
        ::closesocket(sock);
#else
        ::close(sock);
#endif
        return ntohs(bound.sin_port);
    }

    uint16_t reserve_udp_port()
    {
#ifdef _WIN32
        SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET)
            return 0;
#else
        int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
            return 0;
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(sock, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
            ::closesocket(sock);
#else
            ::close(sock);
#endif
            return 0;
        }
        sockaddr_in bound{};
#ifdef _WIN32
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        if (::getsockname(sock, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
#ifdef _WIN32
            ::closesocket(sock);
#else
            ::close(sock);
#endif
            return 0;
        }
#ifdef _WIN32
        ::closesocket(sock);
#else
        ::close(sock);
#endif
        return ntohs(bound.sin_port);
    }

    yuan::coroutine::Task<void> echo_handler(yuan::net::AsyncConnectionContext ctx)
    {
        while (ctx.is_connected()) {
            auto rr = co_await ctx.read_async(3000);
            if (rr.status != yuan::coroutine::IoStatus::success)
                break;
            auto wr = co_await ctx.write_async(rr.data, 3000);
            if (wr.status != yuan::coroutine::IoStatus::success)
                break;
            auto fr = co_await ctx.flush_async(3000);
            if (fr.status != yuan::coroutine::IoStatus::success)
                break;
        }
        co_await ctx.close_async();
    }

    void run_echo_server(uint16_t port)
    {
        yuan::net::NetworkRuntime runtime;
        yuan::net::AsyncListenerHost host;

        host.set_connection_handler([](yuan::net::AsyncConnectionContext ctx)->yuan::coroutine::Task<void> {
        co_await echo_handler(std::move(ctx));
        });

        bool bound = host.bind("127.0.0.1", port, runtime);
        if (!bound)
            return;

        auto accept_task = host.run_async();
        accept_task.resume();

        while (true) {
            runtime.run();
        }
    }

    void run_udp_echo_server(uint16_t port, std::atomic_bool &ready, std::atomic_bool &stopped)
    {
#ifdef _WIN32
        SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET)
            return;
#else
        int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
            return;
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);

        if (::bind(sock, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
            ::closesocket(sock);
#else
            ::close(sock);
#endif
            return;
        }

#ifdef _WIN32
        DWORD timeout_ms = 1500;
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
#else
        timeval timeout{};
        timeout.tv_sec = 1;
        timeout.tv_usec = 500000;
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

        ready.store(true);

        while (!stopped.load()) {
            sockaddr_in client_addr{};
#ifdef _WIN32
            int client_len = sizeof(client_addr);
#else
            socklen_t client_len = sizeof(client_addr);
#endif
            char buf[4096];
            int n = ::recvfrom(sock, buf, sizeof(buf), 0,
                               reinterpret_cast<sockaddr *>(&client_addr), &client_len);
            if (n > 0) {
                ::sendto(sock, buf, n, 0,
                         reinterpret_cast<const sockaddr *>(&client_addr), client_len);
            }
        }

#ifdef _WIN32
        ::closesocket(sock);
#else
        ::close(sock);
#endif
    }

    void test_network_runtime_lifecycle()
    {
        std::cout << "  [NetworkRuntime] lifecycle + schedule\n";

        yuan::net::NetworkRuntime runtime;
        check(runtime.event_loop() != nullptr, "should have event loop");
        check(runtime.timer_manager() != nullptr, "should have timer manager");
        check(runtime.poller() != nullptr, "should have poller");
        check(runtime.owns_loop(), "should own its loop");

        auto rv = runtime.runtime_view();
        check(rv.event_loop() != nullptr, "runtime_view should have event loop");
        check(rv.timer_manager() != nullptr, "runtime_view should have timer manager");

        bool timer_fired = false;
        auto *timer = runtime.schedule(10, [&timer_fired]() {
        timer_fired = true;
        });
        check(timer != nullptr, "schedule should return a timer");

        auto test_fn = [&](yuan::coroutine::RuntimeView rv)->yuan::coroutine::Task<int>
        {
            co_await rv.schedule();
            co_await rv.sleep_for(50);
            check(timer_fired, "scheduled timer should have fired");
            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(rv, test_fn(rv));
        check(result == 0, "runtime lifecycle test should return 0");

        runtime.cancel_timer(timer);
    }

    void test_async_listener_host_bind_close()
    {
        std::cout << "  [AsyncListenerHost] bind + close\n";

        yuan::net::NetworkRuntime runtime;
        yuan::net::AsyncListenerHost host;

        const uint16_t port = reserve_tcp_port();
        check(port != 0, "should reserve a TCP port");

        bool bound = host.bind(port, runtime);
        check(bound, "bind should succeed");
        check(host.is_listening(), "should be listening after bind");
        check(host.runtime() == &runtime, "runtime() should match");

        host.close();
        check(!host.is_listening(), "should not be listening after close");
    }

    void test_async_connection_context_lifecycle()
    {
        std::cout << "  [AsyncConnectionContext] lifecycle\n";

        yuan::net::NetworkRuntime runtime;
        auto rv = runtime.runtime_view();

        yuan::net::AsyncConnectionContext empty_ctx;
        check(!empty_ctx.is_connected(), "default ctx should not be connected");
        check(!static_cast<bool>(empty_ctx), "default ctx should be falsy");

        auto test_default_ctx = [&](yuan::coroutine::RuntimeView rv)->yuan::coroutine::Task<int>
        {
            yuan::net::AsyncConnectionContext ctx;

            auto rr = co_await ctx.read_async();
            check(rr.status == yuan::coroutine::IoStatus::invalid_state,
                  "read on empty ctx should return invalid_state");

            yuan::buffer::ByteBuffer dummy_buf;
            auto wr = co_await ctx.write_async(dummy_buf);
            check(wr.status == yuan::coroutine::IoStatus::invalid_state,
                  "write on empty ctx should return invalid_state");

            auto cr = co_await ctx.close_async();
            check(cr == yuan::coroutine::IoStatus::invalid_state,
                  "close on empty ctx should return invalid_state");

            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(rv, test_default_ctx(rv));
        check(result == 0, "connection context lifecycle test should return 0");
    }

    void test_async_client_session_connect_read_write()
    {
        std::cout << "  [AsyncClientSession] connect + read + write + close\n";

        const uint16_t port = reserve_tcp_port();
        check(port != 0, "should reserve a TCP port");

        std::thread server_thread([&]() {
        run_echo_server(port);
        });
        server_thread.detach();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        yuan::net::NetworkRuntime client_runtime;
        auto client_rv = client_runtime.runtime_view();

        auto client_test = [&](yuan::coroutine::RuntimeView rv)->yuan::coroutine::Task<int>
        {
            co_await rv.schedule();

            yuan::net::AsyncClientSession session;
            bool connected = co_await session.connect_async(rv, "127.0.0.1", port, 3000);
            check(connected, "connect_async should succeed");
            check(session.is_connected(), "session should be connected");

            std::string msg = "hello async";
            yuan::buffer::ByteBuffer write_buf;
            write_buf.append(msg.data(), msg.size());

            auto write_result = co_await session.write_async(write_buf, 3000);
            check(write_result.status == yuan::coroutine::IoStatus::success,
                  "write_async should succeed");

            auto flush_result = co_await session.flush_async(3000);
            check(flush_result.status == yuan::coroutine::IoStatus::success,
                  "flush_async should succeed");

            auto read_result = co_await session.read_async(3000);
            check(read_result.status == yuan::coroutine::IoStatus::success,
                  "read_async should succeed");

            std::string received = buffer_to_string(read_result.data);
            check(received == msg, "echo should match original message");

            auto close_result = co_await session.close_async();
            check(close_result == yuan::coroutine::IoStatus::success,
                  "close_async should succeed");

            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(client_rv, client_test(client_rv));
        check(result == 0, "client session test should return 0");
    }

    void test_async_request_client_one_shot()
    {
        std::cout << "  [AsyncRequestClient] one-shot request\n";

        const uint16_t port = reserve_tcp_port();
        check(port != 0, "should reserve a TCP port");

        std::thread server_thread([&]() {
        run_echo_server(port);
        });
        server_thread.detach();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        yuan::net::NetworkRuntime client_runtime;
        auto client_rv = client_runtime.runtime_view();

        auto client_test = [&](yuan::coroutine::RuntimeView rv)->yuan::coroutine::Task<int>
        {
            co_await rv.schedule();

            yuan::net::AsyncRequestClient req_client(rv);

            std::string msg = "request-response";
            yuan::buffer::ByteBuffer request_buf;
            request_buf.append(msg.data(), msg.size());

            auto result = co_await req_client.request_async(
                "127.0.0.1", port, request_buf, 3000, 3000);

            check(result.status == yuan::coroutine::IoStatus::success,
                  "request_async should succeed");

            std::string received = buffer_to_string(result.data);
            check(received == msg, "echo should match original message");

            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(client_rv, client_test(client_rv));
        check(result == 0, "request client test should return 0");
    }

    void test_async_datagram_client_send_receive()
    {
        std::cout << "  [AsyncDatagramClient] send + receive\n";

        const uint16_t port = reserve_udp_port();
        check(port != 0, "should reserve a UDP port");

        std::atomic_bool server_ready{ false };
        std::atomic_bool server_stopped{ false };

        std::thread server_thread([&]() {
        run_udp_echo_server(port, server_ready, server_stopped);
        });

        for (int i = 0; i < 50 && !server_ready.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(server_ready.load(), "UDP echo server should start");

        yuan::net::NetworkRuntime client_runtime;
        auto client_rv = client_runtime.runtime_view();

        auto client_test = [&](yuan::coroutine::RuntimeView rv)->yuan::coroutine::Task<int>
        {
            co_await rv.schedule();

            yuan::net::AsyncDatagramClient udp_client;
            bool connected = udp_client.connect("127.0.0.1", port, rv);
            check(connected, "datagram client connect should succeed");
            check(udp_client.is_connected(), "datagram client should be connected");

            std::string msg = "hello udp";
            yuan::buffer::ByteBuffer send_buf;
            send_buf.append(msg.data(), msg.size());

            auto send_result = udp_client.send(send_buf);
            check(send_result.status == yuan::coroutine::IoStatus::success,
                  "send should succeed");
            check(send_result.bytes_sent > 0, "send should report bytes sent");

            auto recv_result = co_await udp_client.receive_async(3000);
            check(recv_result.status == yuan::coroutine::IoStatus::success,
                  "receive_async should succeed");

            std::string received = buffer_to_string(recv_result.data);
            check(received == msg, "UDP echo should match original message");

            udp_client.close();
            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(client_rv, client_test(client_rv));
        check(result == 0, "datagram client test should return 0");

        server_stopped.store(true);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

    void test_async_datagram_client_send_and_receive_async()
    {
        std::cout << "  [AsyncDatagramClient] send_and_receive_async\n";

        const uint16_t port = reserve_udp_port();
        check(port != 0, "should reserve a UDP port");

        std::atomic_bool server_ready{ false };
        std::atomic_bool server_stopped{ false };

        std::thread server_thread([&]() {
        run_udp_echo_server(port, server_ready, server_stopped);
        });

        for (int i = 0; i < 50 && !server_ready.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(server_ready.load(), "UDP echo server should start");

        yuan::net::NetworkRuntime client_runtime;
        auto client_rv = client_runtime.runtime_view();

        auto client_test = [&](yuan::coroutine::RuntimeView rv)->yuan::coroutine::Task<int>
        {
            co_await rv.schedule();

            yuan::net::AsyncDatagramClient udp_client;
            bool connected = udp_client.connect("127.0.0.1", port, rv);
            check(connected, "datagram client connect should succeed");

            std::string msg = "send_and_receive";
            yuan::buffer::ByteBuffer send_buf;
            send_buf.append(msg.data(), msg.size());

            auto result = co_await udp_client.send_and_receive_async(send_buf, 3000);
            check(result.status == yuan::coroutine::IoStatus::success,
                  "send_and_receive_async should succeed");

            std::string received = buffer_to_string(result.data);
            check(received == msg, "echo should match original message");

            udp_client.close();
            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(client_rv, client_test(client_rv));
        check(result == 0, "send_and_receive_async test should return 0");

        server_stopped.store(true);
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

    void test_io_awaitables_via_connection_context()
    {
        std::cout << "  [IO awaitables] async_connect + async_read + async_write + async_flush + async_close\n";

        const uint16_t port = reserve_tcp_port();
        check(port != 0, "should reserve a TCP port");

        std::thread server_thread([&]() {
        run_echo_server(port);
        });
        server_thread.detach();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        yuan::net::NetworkRuntime client_runtime;
        auto client_rv = client_runtime.runtime_view();

        auto client_test = [&](yuan::coroutine::RuntimeView rv)->yuan::coroutine::Task<int>
        {
            co_await rv.schedule();

            auto connect_result = co_await yuan::coroutine::async_connect(rv, "127.0.0.1", port, 3000);
            check(connect_result.result == yuan::coroutine::ConnectResult::success,
                  "async_connect should succeed");
            check(connect_result.connection != nullptr, "should return a connection");

            auto conn = connect_result.connection;
            yuan::net::AsyncConnectionContext ctx(conn, rv);

            std::string msg1 = "first write";
            yuan::buffer::ByteBuffer buf1;
            buf1.append(msg1.data(), msg1.size());

            auto wr1 = co_await ctx.write_async(buf1, 3000);
            check(wr1.status == yuan::coroutine::IoStatus::success, "write 1 should succeed");

            auto fr1 = co_await ctx.flush_async(3000);
            check(fr1.status == yuan::coroutine::IoStatus::success, "flush 1 should succeed");

            auto rr1 = co_await ctx.read_async(3000);
            check(rr1.status == yuan::coroutine::IoStatus::success, "read 1 should succeed");
            std::string recv1 = buffer_to_string(rr1.data);
            check(recv1 == msg1, "echo 1 should match");

            std::string msg2 = "second write";
            yuan::buffer::ByteBuffer buf2;
            buf2.append(msg2.data(), msg2.size());

            auto wr2 = co_await ctx.write_async(buf2, 3000);
            check(wr2.status == yuan::coroutine::IoStatus::success, "write 2 should succeed");

            auto fr2 = co_await ctx.flush_async(3000);
            check(fr2.status == yuan::coroutine::IoStatus::success, "flush 2 should succeed");

            auto rr2 = co_await ctx.read_async(3000);
            check(rr2.status == yuan::coroutine::IoStatus::success, "read 2 should succeed");
            std::string recv2 = buffer_to_string(rr2.data);
            check(recv2 == msg2, "echo 2 should match");

            auto cr = co_await ctx.close_async();
            check(cr == yuan::coroutine::IoStatus::success, "close should succeed");

            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(client_rv, client_test(client_rv));
        check(result == 0, "IO awaitables test should return 0");
    }

} // namespace

int main()
{
    const test::common::WinsockGuard winsock;
    if (!winsock.ok()) {
        std::cerr << "winsock init failed\n";
        return 1;
    }

    std::cout << "Running async facades smoke tests...\n\n";

    test_network_runtime_lifecycle();
    test_async_listener_host_bind_close();
    test_async_connection_context_lifecycle();
    test_async_client_session_connect_read_write();
    test_async_request_client_one_shot();
    test_async_datagram_client_send_receive();
    test_async_datagram_client_send_and_receive_async();
    test_io_awaitables_via_connection_context();

    std::cout << "\n";
    if (g_failed > 0) {
        std::cout << "FAILED: " << g_failed << " assertions failed\n";
        return 1;
    }

    std::cout << "All async facades smoke tests passed (" << g_passed << " assertions ok)\n";
    return 0;
}
