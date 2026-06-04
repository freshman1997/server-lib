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
#include "net/connection/connection_handle.h"
#include "net/handler/connection_handler.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/inet_address.h"


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

    class ImmediateFlushConnection final : public yuan::net::Connection
    {
    public:
        ImmediateFlushConnection()
            : remote_addr_("127.0.0.1", 12345),
              local_addr_("127.0.0.1", 23456)
        {
        }

        yuan::net::ConnectionState get_connection_state() const override
        {
            return state_;
        }

        bool is_connected() const override
        {
            return state_ == yuan::net::ConnectionState::connected;
        }

        const yuan::net::InetAddress &get_remote_address() const override
        {
            return remote_addr_;
        }

        const yuan::net::InetAddress &get_local_address() const override
        {
            return local_addr_;
        }

        void write(const ::yuan::buffer::ByteBuffer &buffer) override
        {
            append_output(buffer);
        }

        void write_and_flush(const ::yuan::buffer::ByteBuffer &buffer) override
        {
            write(buffer);
            flush();
        }

        void flush() override
        {
            output_buffer_.clear();
        }

        void abort() override
        {
            state_ = yuan::net::ConnectionState::closed;
        }

        void close() override
        {
            state_ = yuan::net::ConnectionState::closed;
        }

        void set_connection_handler(std::shared_ptr<yuan::net::ConnectionHandler> handler) override
        {
            handler_owner_ = std::move(handler);
            handler_ = handler_owner_.get();
        }

        yuan::net::ConnectionHandler *get_connection_handler() const override
        {
            return handler_;
        }

        std::shared_ptr<yuan::net::ConnectionHandler> get_connection_handler_owner() const override
        {
            return handler_owner_;
        }

        void set_ssl_handler(std::shared_ptr<yuan::net::SSLHandler> sslHandler) override
        {
            ssl_handler_ = std::move(sslHandler);
        }

        void on_read_event() override
        {
        }

        void on_write_event() override
        {
        }

        void set_event_handler(yuan::net::EventHandler *eventHandler) override
        {
            event_handler_ = eventHandler;
        }

        void inject_input(std::string_view text)
        {
            input_buffer_.append(text);
            notify_event_waiters(yuan::net::ConnectionEvent::readable);
        }

    private:
        yuan::net::ConnectionState state_ = yuan::net::ConnectionState::connected;
        yuan::net::InetAddress remote_addr_;
        yuan::net::InetAddress local_addr_;
        yuan::net::ConnectionHandler *handler_ = nullptr;
        std::shared_ptr<yuan::net::ConnectionHandler> handler_owner_;
        yuan::net::EventHandler *event_handler_ = nullptr;
        std::shared_ptr<yuan::net::SSLHandler> ssl_handler_;
    };

    class ClosingConnection final : public yuan::net::Connection
    {
    public:
        ClosingConnection()
            : remote_addr_("127.0.0.1", 12345),
              local_addr_("127.0.0.1", 23456)
        {
        }

        yuan::net::ConnectionState get_connection_state() const override
        {
            return state_;
        }

        bool is_connected() const override
        {
            return state_ == yuan::net::ConnectionState::connected;
        }

        const yuan::net::InetAddress &get_remote_address() const override
        {
            return remote_addr_;
        }

        const yuan::net::InetAddress &get_local_address() const override
        {
            return local_addr_;
        }

        void write(const ::yuan::buffer::ByteBuffer &buffer) override
        {
            append_output(buffer);
        }

        void write_and_flush(const ::yuan::buffer::ByteBuffer &buffer) override
        {
            write(buffer);
            flush();
        }

        void flush() override
        {
            output_buffer_.clear();
        }

        void abort() override
        {
            close();
        }

        void close() override
        {
            if (state_ == yuan::net::ConnectionState::closed) {
                return;
            }
            state_ = yuan::net::ConnectionState::closed;
            notify_event_waiters(yuan::net::ConnectionEvent::closed);
        }

        void set_connection_handler(std::shared_ptr<yuan::net::ConnectionHandler> handler) override
        {
            handler_owner_ = std::move(handler);
            handler_ = handler_owner_.get();
        }

        yuan::net::ConnectionHandler *get_connection_handler() const override
        {
            return handler_;
        }

        std::shared_ptr<yuan::net::ConnectionHandler> get_connection_handler_owner() const override
        {
            return handler_owner_;
        }

        void set_ssl_handler(std::shared_ptr<yuan::net::SSLHandler> sslHandler) override
        {
            ssl_handler_ = std::move(sslHandler);
        }

        void on_read_event() override
        {
        }

        void on_write_event() override
        {
        }

        void set_event_handler(yuan::net::EventHandler *eventHandler) override
        {
            event_handler_ = eventHandler;
        }

    private:
        yuan::net::ConnectionState state_ = yuan::net::ConnectionState::connected;
        yuan::net::InetAddress remote_addr_;
        yuan::net::InetAddress local_addr_;
        yuan::net::ConnectionHandler *handler_ = nullptr;
        std::shared_ptr<yuan::net::ConnectionHandler> handler_owner_;
        yuan::net::EventHandler *event_handler_ = nullptr;
        std::shared_ptr<yuan::net::SSLHandler> ssl_handler_;
    };

    class TrackingHandler final : public yuan::net::ConnectionHandler
    {
    public:
        void on_connected(yuan::net::Connection &) override
        {
            ++connected_count;
        }

        void on_error(yuan::net::Connection &) override
        {
        }

        void on_read(yuan::net::Connection &) override
        {
        }

        void on_write(yuan::net::Connection &) override
        {
        }

        void on_close(yuan::net::Connection &) override
        {
        }

        void on_input_shutdown(yuan::net::Connection &) override
        {
        }

        int connected_count = 0;
    };

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
        auto timer = runtime.schedule_handle(10, [&timer_fired]() {
        timer_fired = true;
        });
        check(static_cast<bool>(timer), "schedule_handle should return a timer handle");

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

    void test_accept_awaitable_does_not_replace_handler()
    {
        std::cout << "  [AcceptAwaitable] accept waiter does not replace handler\n";

        const uint16_t port = reserve_tcp_port();
        check(port != 0, "should reserve a TCP port for accept waiter test");

        yuan::net::NetworkRuntime runtime;
        yuan::net::AsyncListenerHost host;
        bool bound = host.bind("127.0.0.1", port, runtime);
        check(bound, "accept waiter test bind should succeed");

        auto handler = std::make_shared<TrackingHandler>();
        host.acceptor()->set_connection_handler(handler);
        check(host.acceptor()->connection_handler() == handler.get(),
              "acceptor handler should be installed before async_accept");

        std::thread client_thread([port]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                return;
            }
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            (void)::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
#ifdef _WIN32
            ::closesocket(fd);
#else
            ::close(fd);
#endif
        });

        auto test_fn = [&](yuan::coroutine::RuntimeView view)->yuan::coroutine::Task<int>
        {
            co_await view.schedule();
            auto conn = co_await yuan::coroutine::async_accept(view, host.acceptor().get());
            check(conn != nullptr, "async_accept should return accepted connection");
            check(host.acceptor()->connection_handler() == handler.get(),
                  "async_accept should not replace acceptor handler");
            check(handler->connected_count == 1,
                  "original acceptor handler should still observe accepted connection");
            if (conn) {
                conn->close();
            }
            host.close();
            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(runtime.runtime_view(), test_fn(runtime.runtime_view()));
        check(result == 0, "accept waiter preservation test should return 0");
        if (client_thread.joinable()) {
            client_thread.join();
        }
    }

    void test_connect_awaitable_does_not_replace_handler()
    {
        std::cout << "  [ConnectAwaitable] connect waiter uses connection event waiters\n";

        const uint16_t port = reserve_tcp_port();
        check(port != 0, "should reserve a TCP port for connect waiter test");

        std::thread server_thread([&]() {
            int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listen_fd < 0) {
                return;
            }
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
                ::closesocket(listen_fd);
#else
                ::close(listen_fd);
#endif
                return;
            }
            ::listen(listen_fd, 1);
            sockaddr_in peer{};
#ifdef _WIN32
            int peer_len = sizeof(peer);
#else
            socklen_t peer_len = sizeof(peer);
#endif
            int fd = ::accept(listen_fd, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if (fd >= 0) {
#ifdef _WIN32
                ::closesocket(fd);
#else
                ::close(fd);
#endif
            }
#ifdef _WIN32
            ::closesocket(listen_fd);
#else
            ::close(listen_fd);
#endif
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        yuan::net::NetworkRuntime runtime;
        auto test_fn = [&](yuan::coroutine::RuntimeView view)->yuan::coroutine::Task<int>
        {
            co_await view.schedule();
            auto result = co_await yuan::coroutine::async_connect(view, "127.0.0.1", port, 3000);
            check(result.result == yuan::coroutine::ConnectResult::success,
                  "async_connect should complete via connection event waiter");
            check(result.connection != nullptr, "async_connect should return connection");
            if (result.connection) {
                check(result.connection->get_connection_handler() == nullptr,
                      "async_connect should not install proxy connection handler");
                result.connection->close();
            }
            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(runtime.runtime_view(), test_fn(runtime.runtime_view()));
        check(result == 0, "connect waiter preservation test should return 0");
        if (server_thread.joinable()) {
            server_thread.join();
        }
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

        auto conn = std::make_shared<ImmediateFlushConnection>();
        auto existing_handler = std::make_shared<TrackingHandler>();
        conn->set_connection_handler(existing_handler);

        yuan::net::AsyncConnectionContext ctx(conn, rv);
        check(conn->get_connection_handler() == existing_handler.get(),
              "context construction should not replace existing connection handler");

        ctx.install_default_handler();
        check(conn->get_connection_handler() != existing_handler.get(),
              "install_default_handler should explicitly replace the handler");

        auto limited_conn = std::make_shared<ImmediateFlushConnection>();
        limited_conn->set_max_output_buffer_size(4);
        yuan::net::AsyncConnectionContext limited_ctx(limited_conn, rv);
        limited_ctx.append_output("too-large");
        check(limited_conn->output_limit_exceeded(),
              "append_output should mark output limit overflow");
        check(!limited_ctx.is_connected(),
              "append_output overflow should close the connection context");

        auto test_async_read_keeps_handler = [&](yuan::coroutine::RuntimeView view)->yuan::coroutine::Task<int>
        {
            auto read_conn = std::make_shared<ImmediateFlushConnection>();
            auto handler = std::make_shared<TrackingHandler>();
            read_conn->set_connection_handler(handler);
            yuan::net::ConnectionHandle handle(read_conn);

            view.schedule(10, [read_conn]() {
                read_conn->inject_input("ready");
            });

            auto result = co_await view.read(handle, 100);
            check(result.status == yuan::coroutine::IoStatus::success,
                  "async read via waiter should succeed");
            check(buffer_to_string(result.data) == "ready",
                  "async read via waiter should return injected input");
            check(read_conn->get_connection_handler() == handler.get(),
                  "async read should not replace existing connection handler");
            co_return 0;
        };

        const int read_result = yuan::coroutine::sync_wait(rv, test_async_read_keeps_handler(rv));
        check(read_result == 0, "async read handler preservation test should return 0");
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
            check(session.context().connection()->get_remote_address().get_port() == port,
                  "remote address should match server port");
            check(session.context().connection()->get_local_address().get_port() != port,
                  "local address should be client ephemeral port, not remote port");

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

    void test_write_and_flush_immediate_completion_regression()
    {
        std::cout << "  [IO awaitables] immediate write/flush completion regression\n";

        yuan::net::NetworkRuntime runtime;
        auto rv = runtime.runtime_view();
        auto conn = std::make_shared<ImmediateFlushConnection>();

        auto test_fn = [&](yuan::coroutine::RuntimeView view)->yuan::coroutine::Task<int>
        {
            co_await view.schedule();
            yuan::net::ConnectionHandle conn_handle(conn);

            yuan::buffer::ByteBuffer write_buf;
            write_buf.append("ping", 4);

            auto write_result = co_await view.write(conn_handle, write_buf, 100);
            check(write_result.status == yuan::coroutine::IoStatus::success,
                  "async_write should complete even if flush finishes synchronously");
            check(conn->output_readable_bytes() == 0,
                  "synchronous flush should drain output buffer");

            conn->append_output("pong", 4);
            auto flush_result = co_await view.flush(conn_handle, 100);
            check(flush_result.status == yuan::coroutine::IoStatus::success,
                  "async_flush should complete even if no on_write callback fires");
            check(conn->output_readable_bytes() == 0,
                  "async_flush should leave no pending output");

            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(rv, test_fn(rv));
        check(result == 0, "immediate completion regression test should return 0");
    }

    void test_async_read_handle_keeps_connection_until_close()
    {
        std::cout << "  [IO awaitables] read handle owns connection until close\n";

        yuan::net::NetworkRuntime runtime;
        auto rv = runtime.runtime_view();
        std::weak_ptr<ClosingConnection> weak_conn;

        auto test_fn = [&](yuan::coroutine::RuntimeView view)->yuan::coroutine::Task<int>
        {
            co_await view.schedule();
            auto conn = std::make_shared<ClosingConnection>();
            weak_conn = conn;
            yuan::net::ConnectionHandle handle(conn);

            view.schedule(10, [conn]() {
                conn->close();
            });
            conn.reset();

            auto result = co_await view.read(handle, 100);
            check(result.status == yuan::coroutine::IoStatus::connection_closed,
                  "async read should resume with connection_closed after delayed close");
            check(!weak_conn.expired(),
                  "ConnectionHandle should keep connection alive across suspended read");
            handle = yuan::net::ConnectionHandle();
            co_await view.schedule();
            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(rv, test_fn(rv));
        check(result == 0, "read handle ownership regression test should return 0");
        check(weak_conn.expired(), "completed timer callbacks should release captured connection ownership");
    }

    void test_async_read_timeout_then_late_event()
    {
        std::cout << "  [IO awaitables] read timeout ignores late event\n";

        yuan::net::NetworkRuntime runtime;
        auto rv = runtime.runtime_view();
        auto conn = std::make_shared<ImmediateFlushConnection>();

        auto test_fn = [&](yuan::coroutine::RuntimeView view)->yuan::coroutine::Task<int>
        {
            co_await view.schedule();
            yuan::net::ConnectionHandle handle(conn);

            auto result = co_await view.read(handle, 10);
            check(result.status == yuan::coroutine::IoStatus::timed_out,
                  "async read should return timed_out before data arrives");

            conn->inject_input("late");
            co_await view.schedule();
            check(conn->input_readable_bytes() == 4,
                  "late input should remain buffered after timed-out awaiter is cancelled");
            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(rv, test_fn(rv));
        check(result == 0, "read timeout late event regression should return 0");
    }

    void test_multiple_read_waiters_rejected()
    {
        std::cout << "  [IO awaitables] multiple read waiters are rejected\n";

        yuan::net::NetworkRuntime runtime;
        auto rv = runtime.runtime_view();
        auto conn = std::make_shared<ImmediateFlushConnection>();

        auto pending_read = [&](yuan::coroutine::RuntimeView view,
                                yuan::net::ConnectionHandle handle,
                                bool &started)->yuan::coroutine::Task<yuan::coroutine::ReadResult>
        {
            started = true;
            auto result = co_await view.read(handle, 100);
            co_return result;
        };

        auto test_fn = [&](yuan::coroutine::RuntimeView view)->yuan::coroutine::Task<int>
        {
            co_await view.schedule();
            yuan::net::ConnectionHandle handle(conn);
            bool first_started = false;
            auto first = pending_read(view, handle, first_started);
            first.resume();
            check(first_started, "first read coroutine should start");
            check(conn->has_event_waiter(yuan::net::ConnectionEvent::readable),
                  "first read should register readable waiter");

            auto second = co_await view.read(handle, 100);
            check(second.status == yuan::coroutine::IoStatus::invalid_state,
                  "second concurrent read waiter should be rejected");

            conn->inject_input("one");
            co_await view.schedule();
            check(first.done(), "first read coroutine should complete after input arrives");
            auto first_result = first.resume_once_and_get_result();
            check(first_result.status == yuan::coroutine::IoStatus::success,
                  "first read waiter should still complete");
            check(buffer_to_string(first_result.data) == "one",
                  "first read waiter should receive injected data");
            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(rv, test_fn(rv));
        check(result == 0, "multiple read waiter policy test should return 0");
    }

    void test_async_waiters_do_not_replace_handler()
    {
        std::cout << "  [IO awaitables] waiters do not replace handler\n";

        yuan::net::NetworkRuntime runtime;
        auto rv = runtime.runtime_view();
        auto conn = std::make_shared<ImmediateFlushConnection>();
        auto handler = std::make_shared<TrackingHandler>();
        conn->set_connection_handler(handler);

        auto test_fn = [&](yuan::coroutine::RuntimeView view)->yuan::coroutine::Task<int>
        {
            co_await view.schedule();
            yuan::net::ConnectionHandle handle(conn);
            check(conn->get_connection_handler() == handler.get(),
                  "handler should be installed before awaiters");

            conn->append_output("flush", 5);
            auto flush_result = co_await view.flush(handle, 100);
            check(flush_result.status == yuan::coroutine::IoStatus::success,
                  "flush waiter should complete");
            check(conn->get_connection_handler() == handler.get(),
                  "flush waiter should not replace handler");

            view.schedule(10, [conn]() {
                conn->inject_input("read");
            });
            auto read_result = co_await view.read(handle, 100);
            check(read_result.status == yuan::coroutine::IoStatus::success,
                  "read waiter should complete");
            check(conn->get_connection_handler() == handler.get(),
                  "read waiter should not replace handler");

            auto close_result = co_await view.close(handle);
            check(close_result == yuan::coroutine::IoStatus::success,
                  "close waiter should complete");
            check(conn->get_connection_handler() == handler.get(),
                  "close waiter should not replace handler");

            auto local_addr = conn->get_local_address();
            check(local_addr.get_ip() == "127.0.0.1" || !local_addr.get_ip().empty(),
                  "local address should be populated before close");
            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(rv, test_fn(rv));
        check(result == 0, "handler preservation regression should return 0");
    }

} // namespace

int main()
{
    std::cout << "Running async facades smoke tests...\n\n";

    test_network_runtime_lifecycle();
    test_async_listener_host_bind_close();
    test_accept_awaitable_does_not_replace_handler();
    test_connect_awaitable_does_not_replace_handler();
    test_async_connection_context_lifecycle();
    test_async_client_session_connect_read_write();
    test_async_request_client_one_shot();
    test_async_datagram_client_send_receive();
    test_async_datagram_client_send_and_receive_async();
    test_io_awaitables_via_connection_context();
    test_write_and_flush_immediate_completion_regression();
    test_async_read_handle_keeps_connection_until_close();
    test_async_read_timeout_then_late_event();
    test_multiple_read_waiters_rejected();
    test_async_waiters_do_not_replace_handler();

    std::cout << "\n";
    if (g_failed > 0) {
        std::cout << "FAILED: " << g_failed << " assertions failed\n";
        return 1;
    }

    std::cout << "All async facades smoke tests passed (" << g_passed << " assertions ok)\n";
    return 0;
}
