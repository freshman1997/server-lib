#include "coroutine/runtime.h"
#include "coroutine/sync_wait.h"
#include "coroutine/task.h"
#include "net/async/async_client_session.h"
#include "net/async/async_listener_host.h"
#include "net/acceptor/datagram_endpoint.h"
#include "net/acceptor/udp/udp_instance.h"
#include "net/connection/connection_factory.h"
#include "net/connection/connection_handle.h"
#include "net/connection/datagram_transport.h"
#include "net/connection/udp_connection.h"
#include "net/runtime/network_runtime.h"
#include "net/channel/channel.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "timer/wheel_timer_manager.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <atomic>

#ifdef _WIN32
#include <arpa/inet.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
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
            std::cerr << "  FAIL: " << message << "\n";
            ++g_failed;
        }
    }

    uint16_t reserve_tcp_port()
    {
#ifdef _WIN32
        SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
            return 0;
        }
#else
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            return 0;
        }
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

    void close_fd(int fd)
    {
#ifdef _WIN32
        ::closesocket(fd);
#else
        ::close(fd);
#endif
    }

    void set_recv_timeout(int fd, int seconds)
    {
#ifdef _WIN32
        const DWORD timeout_ms = static_cast<DWORD>(seconds * 1000);
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
#else
        timeval tv{};
        tv.tv_sec = seconds;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    bool recv_exact(int fd, std::string &out, std::size_t expected)
    {
        out.clear();
        out.reserve(expected);
        char buf[4096];
        while (out.size() < expected) {
            const std::size_t remaining = expected - out.size();
            const int n = ::recv(fd, buf, static_cast<int>(std::min<std::size_t>(sizeof(buf), remaining)), 0);
            if (n <= 0) {
                return false;
            }
            out.append(buf, static_cast<std::size_t>(n));
        }
        return true;
    }

    class CountingConnectionHandler : public yuan::net::ConnectionHandler
    {
    public:
        void on_connected(const std::shared_ptr<yuan::net::Connection> &conn) override { (void)conn; ++connected; }
        void on_error(const std::shared_ptr<yuan::net::Connection> &conn) override { (void)conn; ++errors; }
        void on_read(const std::shared_ptr<yuan::net::Connection> &conn) override { (void)conn; ++reads; }
        void on_write(const std::shared_ptr<yuan::net::Connection> &conn) override { (void)conn; ++writes; }
        void on_close(const std::shared_ptr<yuan::net::Connection> &conn) override { (void)conn; ++closes; }

        int connected = 0;
        int errors = 0;
        int reads = 0;
        int writes = 0;
        int closes = 0;
    };

    class FakeDatagramEndpoint : public yuan::net::DatagramEndpoint
    {
    public:
        explicit FakeDatagramEndpoint(yuan::timer::TimerManager *timer_manager)
            : timer_manager_(timer_manager)
        {
        }

        int send_datagram(const std::shared_ptr<yuan::net::Connection> &conn, const yuan::buffer::ByteBuffer &buff) override
        {
            (void)conn;
            sent += std::string(buff.readable_span().begin(), buff.readable_span().end());
            return static_cast<int>(buff.readable_bytes());
        }

        int send_datagram(const yuan::net::InetAddress &addr, const yuan::buffer::ByteBuffer &buff) override
        {
            (void)addr;
            sent += std::string(buff.readable_span().begin(), buff.readable_span().end());
            return static_cast<int>(buff.readable_bytes());
        }

        yuan::net::Channel *endpoint_channel() const override { return nullptr; }
        void update_endpoint_channel() override { ++updates; }
        yuan::timer::TimerManager *endpoint_timer_manager() const override { return timer_manager_; }

        std::string sent;
        int updates = 0;
        yuan::timer::TimerManager *timer_manager_ = nullptr;
    };

    void test_tcp_close_and_address()
    {
        std::cout << "  [TcpConnection] close + address semantics\n";

        const uint16_t port = reserve_tcp_port();
        check(port != 0, "should reserve a TCP port");

        std::thread srv([&]() {
            int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listen_fd < 0) {
                return;
            }
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
                ::close(listen_fd);
                return;
            }
            ::listen(listen_fd, 1);

            sockaddr_in peer{};
#ifdef _WIN32
            int peer_len = sizeof(peer);
#else
            socklen_t peer_len = sizeof(peer);
#endif
            int client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if (client_fd >= 0) {
                char buf[4096];
                int n = ::recv(client_fd, buf, sizeof(buf), 0);
                if (n > 0) {
                    ::send(client_fd, buf, n, 0);
                }
                ::shutdown(client_fd, SHUT_WR);
                ::close(client_fd);
            }
            ::close(listen_fd);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        yuan::net::NetworkRuntime runtime;
        auto rv = runtime.runtime_view();

        auto test_fn = [&](yuan::coroutine::RuntimeView view)->yuan::coroutine::Task<int>
        {
            co_await view.schedule();
            yuan::net::AsyncClientSession session;
            bool connected = co_await session.connect_async(view, "127.0.0.1", port, 3000);
            check(connected, "connect_async should succeed");

            auto conn = session.context().connection();
            check(conn != nullptr, "should have connection");
            check(conn->get_local_address().get_port() != port, "local port should not equal remote port");

            yuan::buffer::ByteBuffer payload;
            payload.append("drain", 5);
            auto wr = co_await session.write_async(payload, 3000);
            check(wr.status == yuan::coroutine::IoStatus::success, "write_async should succeed");
            auto fr = co_await session.flush_async(3000);
            check(fr.status == yuan::coroutine::IoStatus::success, "flush_async should succeed");

            auto rr = co_await session.read_async(3000);
            check(rr.status == yuan::coroutine::IoStatus::success, "read_async should succeed");
            check(std::string(rr.data.readable_span().begin(), rr.data.readable_span().end()) == "drain",
                  "echo should match payload");

            auto cr = co_await session.close_async();
            check(cr == yuan::coroutine::IoStatus::success, "close_async should succeed");
            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(rv, test_fn(rv));
        check(result == 0, "tcp close semantics coroutine should return 0");

        if (srv.joinable()) {
            srv.join();
        }
    }

    void test_write_then_close_drains_large_payload()
    {
        std::cout << "  [TcpConnection] write then close drains payload\n";

        const uint16_t port = reserve_tcp_port();
        check(port != 0, "should reserve a TCP port for drain test");
        constexpr std::size_t payload_size = 512 * 1024;
        std::string received;

        std::thread srv([&]() {
            int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listen_fd < 0) {
                return;
            }
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
                close_fd(listen_fd);
                return;
            }
            ::listen(listen_fd, 1);

            sockaddr_in peer{};
#ifdef _WIN32
            int peer_len = sizeof(peer);
#else
            socklen_t peer_len = sizeof(peer);
#endif
            int client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if (client_fd >= 0) {
                set_recv_timeout(client_fd, 5);
                (void)recv_exact(client_fd, received, payload_size);
                close_fd(client_fd);
            }
            close_fd(listen_fd);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        yuan::net::NetworkRuntime runtime;
        auto rv = runtime.runtime_view();

        auto test_fn = [&](yuan::coroutine::RuntimeView view)->yuan::coroutine::Task<int>
        {
            co_await view.schedule();
            yuan::net::AsyncClientSession session;
            bool connected = co_await session.connect_async(view, "127.0.0.1", port, 3000);
            check(connected, "drain test connect_async should succeed");

            yuan::buffer::ByteBuffer payload;
            std::string text(payload_size, 'x');
            payload.append(text.data(), text.size());

            auto wr = co_await session.write_async(payload, 3000);
            check(wr.status == yuan::coroutine::IoStatus::success, "large write_async should succeed");
            auto cr = co_await session.close_async();
            check(cr == yuan::coroutine::IoStatus::success, "close_async should drain queued output");
            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(rv, test_fn(rv));
        check(result == 0, "large drain coroutine should return 0");

        if (srv.joinable()) {
            srv.join();
        }
        check(received.size() == payload_size, "server should receive full payload before close");
    }

    void test_peer_half_close_allows_write_response()
    {
        std::cout << "  [TcpConnection] peer half-close allows response\n";

        const uint16_t port = reserve_tcp_port();
        check(port != 0, "should reserve a TCP port for half-close test");

        std::thread srv([&]() {
            int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listen_fd < 0) {
                return;
            }
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
                close_fd(listen_fd);
                return;
            }
            ::listen(listen_fd, 1);
            sockaddr_in peer{};
#ifdef _WIN32
            int peer_len = sizeof(peer);
#else
            socklen_t peer_len = sizeof(peer);
#endif
            int client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if (client_fd < 0) {
                close_fd(listen_fd);
                return;
            }

            set_recv_timeout(client_fd, 5);
            const char request[] = "request";
            (void)::send(client_fd, request, sizeof(request) - 1, 0);
            (void)::shutdown(client_fd, SHUT_WR);
            std::string response;
            const bool got_response = recv_exact(client_fd, response, 8);
            check(got_response, "server should receive response after half-close");
            check(response == "response", "half-close response should match");
            close_fd(client_fd);
            close_fd(listen_fd);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        yuan::net::NetworkRuntime runtime;
        auto rv = runtime.runtime_view();
        auto test_fn = [&](yuan::coroutine::RuntimeView view)->yuan::coroutine::Task<int>
        {
            co_await view.schedule();
            yuan::net::AsyncClientSession session;
            bool connected = co_await session.connect_async(view, "127.0.0.1", port, 3000);
            check(connected, "half-close connect_async should succeed");

            auto rr = co_await session.read_async(3000);
            check(rr.status == yuan::coroutine::IoStatus::success, "half-close read_async should receive peer data");
            check(std::string(rr.data.readable_span().begin(), rr.data.readable_span().end()) == "request",
                  "half-close request should match");

            std::optional<yuan::coroutine::ReadResult> terminal;
            for (int i = 0; i < 3; ++i) {
                auto next = co_await session.read_async(1000, false);
                if (next.data.readable_bytes() == 0) {
                    terminal = std::move(next);
                    break;
                }
            }
            check(terminal.has_value(), "second read should eventually observe peer input shutdown or wait without closing output");
            check(terminal->status == yuan::coroutine::IoStatus::success ||
                      terminal->status == yuan::coroutine::IoStatus::connection_closed ||
                      terminal->status == yuan::coroutine::IoStatus::connection_error ||
                      terminal->status == yuan::coroutine::IoStatus::invalid_state ||
                      terminal->status == yuan::coroutine::IoStatus::timed_out,
                  "terminal half-close read should report EOF, close, error, or timeout without closing output");

            yuan::buffer::ByteBuffer response;
            response.append("response", 8);
            auto wr = co_await session.write_async(response, 3000);
            check(wr.status == yuan::coroutine::IoStatus::success, "write after peer half-close should succeed");
            auto fr = co_await session.flush_async(3000);
            check(fr.status == yuan::coroutine::IoStatus::success, "flush after peer half-close should succeed");
            session.close();
            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(rv, test_fn(rv));
        check(result == 0, "half-close coroutine should return 0");

        if (srv.joinable()) {
            srv.join();
        }
    }

    void test_close_and_abort_are_idempotent()
    {
        std::cout << "  [TcpConnection] close and abort are idempotent\n";

        const uint16_t port = reserve_tcp_port();
        check(port != 0, "should reserve a TCP port for idempotent test");

        std::thread srv([&]() {
            int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listen_fd < 0) {
                return;
            }
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
                close_fd(listen_fd);
                return;
            }
            ::listen(listen_fd, 1);
            sockaddr_in peer{};
#ifdef _WIN32
            int peer_len = sizeof(peer);
#else
            socklen_t peer_len = sizeof(peer);
#endif
            int client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if (client_fd >= 0) {
                char buf[64];
                (void)::recv(client_fd, buf, sizeof(buf), 0);
                close_fd(client_fd);
            }
            close_fd(listen_fd);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        yuan::net::NetworkRuntime runtime;
        auto rv = runtime.runtime_view();

        auto test_fn = [&](yuan::coroutine::RuntimeView view)->yuan::coroutine::Task<int>
        {
            co_await view.schedule();
            yuan::net::AsyncClientSession session;
            bool connected = co_await session.connect_async(view, "127.0.0.1", port, 3000);
            check(connected, "idempotent test connect_async should succeed");
            auto conn = session.context().connection();
            check(conn != nullptr, "idempotent test should have connection");
            session.close();
            session.close();
            if (conn) {
                conn->abort();
                conn->close();
            }
            co_await view.schedule();
            check(!conn || conn->get_connection_state() == yuan::net::ConnectionState::closed ||
                            conn->get_connection_state() == yuan::net::ConnectionState::closing,
                  "repeated close/abort should leave connection closing or closed");
            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(rv, test_fn(rv));
        check(result == 0, "idempotent close coroutine should return 0");
        if (srv.joinable()) {
            srv.join();
        }
    }

    void test_close_while_connecting_resumes()
    {
        std::cout << "  [TcpConnection] close while connecting resumes\n";

        auto socket = std::make_unique<yuan::net::Socket>("127.0.0.1", 9);
        check(socket->valid(), "connecting close test socket should be valid");
        auto conn = yuan::net::create_stream_connection(socket.release());
        check(conn != nullptr, "connecting close test should create connection");
        check(conn->get_connection_state() == yuan::net::ConnectionState::connecting,
              "new stream connection should start in connecting state");

        yuan::net::NetworkRuntime runtime;
        auto rv = runtime.runtime_view();

        auto test_fn = [&](yuan::coroutine::RuntimeView view)->yuan::coroutine::Task<int>
        {
            co_await view.schedule();
            yuan::net::ConnectionHandle handle(conn);
            view.register_connection(conn, nullptr);
            auto status = co_await view.close(handle);
            check(status == yuan::coroutine::IoStatus::success,
                  "close while connecting should resume close awaiter");
            check(conn->get_connection_state() == yuan::net::ConnectionState::closing ||
                      conn->get_connection_state() == yuan::net::ConnectionState::closed,
                  "close while connecting should leave connection closing or closed");
            co_return 0;
        };

        const int result = yuan::coroutine::sync_wait(rv, test_fn(rv));
        check(result == 0, "close while connecting coroutine should return 0");
    }

    void test_udp_close_abort_idle_semantics()
    {
        std::cout << "  [UdpConnection] close/abort/idle close semantics\n";

        yuan::timer::WheelTimerManager timers;
        FakeDatagramEndpoint endpoint(&timers);
        yuan::net::UdpInstance instance(&endpoint);
        const yuan::net::InetAddress peer("127.0.0.1", 53000);

        auto graceful = yuan::net::create_datagram_connection(peer);
        auto graceful_datagram = std::dynamic_pointer_cast<yuan::net::DatagramTransport>(graceful);
        check(graceful != nullptr && graceful_datagram != nullptr, "should create UDP datagram connection");
        CountingConnectionHandler graceful_handler;
        graceful->set_connection_handler(yuan::net::make_non_owning_handler(graceful_handler));
        graceful_datagram->attach_datagram_instance(&instance);
        graceful_datagram->set_datagram_state(yuan::net::ConnectionState::connected);
        yuan::buffer::ByteBuffer queued;
        queued.append("queued", 6);
        graceful->write(queued);
        graceful->close();
        check(endpoint.sent == "queued", "UDP graceful close should drain queued datagram");
        check(graceful->get_connection_state() == yuan::net::ConnectionState::closing,
              "UDP graceful close should enter closing state before cleanup");
        graceful->close();
        check(graceful_handler.closes == 1, "UDP graceful close should notify once");

        endpoint.sent.clear();
        auto aborted = yuan::net::create_datagram_connection(peer);
        auto aborted_datagram = std::dynamic_pointer_cast<yuan::net::DatagramTransport>(aborted);
        CountingConnectionHandler abort_handler;
        aborted->set_connection_handler(yuan::net::make_non_owning_handler(abort_handler));
        aborted_datagram->attach_datagram_instance(&instance);
        aborted_datagram->set_datagram_state(yuan::net::ConnectionState::connected);
        yuan::buffer::ByteBuffer drop;
        drop.append("drop", 4);
        aborted->write(drop);
        aborted->abort();
        aborted->close();
        check(endpoint.sent.empty(), "UDP abort should discard queued datagram");
        check(aborted->get_connection_state() == yuan::net::ConnectionState::closed,
              "UDP abort should move directly to closed state");
        check(abort_handler.closes == 1, "UDP abort followed by close should notify once");

        auto idle = yuan::net::create_datagram_connection(peer);
        auto idle_datagram = std::dynamic_pointer_cast<yuan::net::DatagramTransport>(idle);
        CountingConnectionHandler idle_handler;
        idle->set_connection_handler(yuan::net::make_non_owning_handler(idle_handler));
        idle_datagram->attach_datagram_instance(&instance);
        idle_datagram->set_datagram_state(yuan::net::ConnectionState::connected);
        auto idle_udp = std::dynamic_pointer_cast<yuan::net::UdpConnection>(idle);
        check(idle_udp != nullptr, "idle test should have concrete UDP connection");
        idle_udp->on_timer(nullptr);
        idle_udp->on_timer(nullptr);
        idle_udp->on_timer(nullptr);
        check(idle->get_connection_state() == yuan::net::ConnectionState::closing,
              "UDP idle expiry should use graceful close state");
        check(idle_handler.closes == 1, "UDP idle close should notify once");
    }
}

int main()
{
    test_tcp_close_and_address();
    test_write_then_close_drains_large_payload();
    test_peer_half_close_allows_write_response();
    test_close_and_abort_are_idempotent();
    test_close_while_connecting_resumes();
    test_udp_close_abort_idle_semantics();
    if (g_failed > 0) {
        std::cout << "FAILED: " << g_failed << " assertions failed\n";
        return 1;
    }
    std::cout << "All tcp close semantics tests passed (" << g_passed << " assertions ok)\n";
    return 0;
}
