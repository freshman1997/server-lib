#include "net/iocp/iocp_accept.h"
#include "net/iocp/iocp_completion_port.h"
#include "net/iocp/iocp_connect.h"
#include "net/iocp/iocp_dispatcher.h"
#include "net/iocp/iocp_operation.h"
#include "net/iocp/iocp_socket_context.h"
#include "net/iocp/iocp_tcp_engine.h"
#include "net/iocp/iocp_tcp_io.h"
#include "net/async/async_listener_host.h"
#include "net/handler/connection_handler.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/listen_options.h"
#include "net/socket/socket_ops.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

#ifdef _WIN32
    class RecordingConnectionHandler : public yuan::net::ConnectionHandler
    {
    public:
        std::atomic_bool connected{false};
        std::atomic_bool readable{false};
        std::atomic_bool writable{false};
        std::atomic_bool closed{false};
        std::atomic_bool closed_state{false};

        void on_connected(yuan::net::Connection &) override
        {
            connected.store(true, std::memory_order_release);
        }

        void on_error(yuan::net::Connection &) override
        {
        }

        void on_read(yuan::net::Connection &conn) override
        {
            if (conn.input_readable_bytes() >= 5) {
                (void)conn.take_input_byte_buffer();
                readable.store(true, std::memory_order_release);
            }
        }

        void on_write(yuan::net::Connection &) override
        {
            writable.store(true, std::memory_order_release);
        }

        void on_close(yuan::net::Connection &conn) override
        {
            closed_state.store(conn.get_connection_state() == yuan::net::ConnectionState::closed,
                               std::memory_order_release);
            closed.store(true, std::memory_order_release);
        }

        void on_input_shutdown(yuan::net::Connection &) override
        {
        }
    };

#include <windows.h>
#endif

namespace
{
    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

#ifdef _WIN32
    uint16_t reserve_tcp_port()
    {
        const int fd = yuan::net::socket::create_ipv4_tcp_socket(false);
        if (fd < 0) {
            return 0;
        }
        if (yuan::net::socket::bind(fd, yuan::net::InetAddress("127.0.0.1", 0)) != 0 ||
            yuan::net::socket::listen(fd, 1) != 0) {
            yuan::net::socket::close_fd(fd);
            return 0;
        }
        const auto addr = yuan::net::socket::get_local_address(fd);
        const auto port = static_cast<uint16_t>(addr.get_port());
        yuan::net::socket::close_fd(fd);
        return port;
    }

    void set_recv_timeout(int fd, int milliseconds)
    {
        const DWORD timeout = static_cast<DWORD>(milliseconds);
        (void)::setsockopt(static_cast<SOCKET>(fd),
                           SOL_SOCKET,
                           SO_RCVTIMEO,
                           reinterpret_cast<const char *>(&timeout),
                           sizeof(timeout));
    }

    void shutdown_send(int fd)
    {
        (void)::shutdown(static_cast<SOCKET>(fd), SD_SEND);
    }

    bool test_dispatcher_post_and_stop()
    {
        yuan::net::IocpCompletionPort port;
        if (!require(port.init(), "dispatcher iocp init should succeed")) {
            return false;
        }

        std::mutex mutex;
        std::condition_variable cond;
        std::atomic_bool received{false};
        int marker = 0;
        yuan::net::IocpDispatcher dispatcher;
        if (!require(dispatcher.start(port, 2, [&](const yuan::net::IocpCompletion &completion) {
                if (completion.operation == &marker && completion.key == 0x77 && completion.bytes == 9) {
                    received.store(true, std::memory_order_release);
                    cond.notify_one();
                }
            }), "dispatcher start should succeed")) {
            return false;
        }

        if (!require(dispatcher.running(), "dispatcher should report running")) {
            dispatcher.stop();
            return false;
        }
        if (!require(dispatcher.worker_count() == 2, "dispatcher worker count should match")) {
            dispatcher.stop();
            return false;
        }
        if (!require(port.post(0x77, &marker, 9), "dispatcher test post should succeed")) {
            dispatcher.stop();
            return false;
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!cond.wait_for(lock, std::chrono::seconds(1), [&]() {
                    return received.load(std::memory_order_acquire);
                })) {
                dispatcher.stop();
                return require(false, "dispatcher should deliver posted completion");
            }
        }

        dispatcher.stop();
        if (!require(!dispatcher.running(), "dispatcher should stop")) {
            return false;
        }
        if (!require(dispatcher.worker_count() == 0, "dispatcher workers should join")) {
            return false;
        }
        return true;
    }

    bool test_dispatcher_operation_callback()
    {
        yuan::net::IocpCompletionPort port;
        if (!require(port.init(), "operation dispatcher iocp init should succeed")) {
            return false;
        }

        std::mutex mutex;
        std::condition_variable cond;
        std::atomic_bool received{false};
        int owner = 0;
        yuan::net::IocpOperation operation;
        operation.reset(yuan::net::IocpOperationKind::user, &owner, &operation, 42);

        yuan::net::IocpDispatcher dispatcher;
        if (!require(dispatcher.start_operations(port, 1, [&](yuan::net::IocpOperation &completed,
                                                              const yuan::net::IocpCompletion &completion) {
                if (&completed == &operation &&
                    completed.owner == &owner &&
                    completed.user_data == &operation &&
                    completed.generation == 42 &&
                    completion.bytes == 3) {
                    received.store(true, std::memory_order_release);
                    cond.notify_one();
                }
            }), "operation dispatcher start should succeed")) {
            return false;
        }

        if (!require(port.post(0x88, operation.native_overlapped(), 3),
                     "operation dispatcher post should succeed")) {
            dispatcher.stop();
            return false;
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!cond.wait_for(lock, std::chrono::seconds(1), [&]() {
                    return received.load(std::memory_order_acquire);
                })) {
                dispatcher.stop();
                return require(false, "operation dispatcher should deliver operation");
            }
        }

        dispatcher.stop();
        return true;
    }

    bool test_accept_ex_round_trip()
    {
        yuan::net::IocpCompletionPort port;
        if (!require(port.init(), "accept iocp init should succeed")) {
            return false;
        }

        const int listener = yuan::net::socket::create_ipv4_overlapped_tcp_socket(false);
        if (!require(listener >= 0, "overlapped listener socket should be created")) {
            return false;
        }
        yuan::net::socket::set_reuse_addr(listener, true);
        if (!require(yuan::net::socket::bind(listener, yuan::net::InetAddress("127.0.0.1", 0)) == 0,
                     "listener bind should succeed")) {
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(yuan::net::socket::listen(listener, 16) == 0, "listener listen should succeed")) {
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(port.associate_socket(listener, 0xfeed), "listener should associate with iocp")) {
            yuan::net::socket::close_fd(listener);
            return false;
        }

        yuan::net::IocpAcceptEx accept_ex;
        if (!require(accept_ex.load(listener), "AcceptEx functions should load")) {
            yuan::net::socket::close_fd(listener);
            return false;
        }

        const int accepted = yuan::net::socket::create_ipv4_overlapped_tcp_socket(false);
        if (!require(accepted >= 0, "overlapped accepted socket should be created")) {
            yuan::net::socket::close_fd(listener);
            return false;
        }

        OVERLAPPED overlapped{};
        std::array<char, yuan::net::kIocpAcceptBufferBytes> address_buffer{};
        if (!require(accept_ex.post(listener, accepted, address_buffer.data(), address_buffer.size(), &overlapped),
                     "AcceptEx post should succeed or pend")) {
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        const auto listen_addr = yuan::net::socket::get_local_address(listener);
        const int client = yuan::net::socket::create_ipv4_tcp_socket(false);
        if (!require(client >= 0, "client socket should be created")) {
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(yuan::net::socket::connect(client, listen_addr) == 0, "client connect should succeed")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        yuan::net::IocpCompletion completion;
        if (!require(port.wait(1000, completion), "AcceptEx completion should arrive")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(completion.ok, "AcceptEx completion should be ok")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(completion.operation == &overlapped, "AcceptEx operation should round-trip")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(accept_ex.update_accept_context(accepted, listener),
                     "accepted socket context should update")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        yuan::net::IocpAcceptedAddresses addresses;
        if (!require(accept_ex.parse_addresses(address_buffer.data(), address_buffer.size(), addresses),
                     "AcceptEx addresses should parse")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        yuan::net::IocpSocketContext accepted_context;
        if (!require(accepted_context.attach(accepted, port, 0xbeef),
                     "accepted socket context should associate with iocp")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(accepted_context.valid() &&
                     accepted_context.fd() == accepted &&
                     accepted_context.key() == 0xbeef &&
                     accepted_context.pending_operations() == 0,
                     "accepted socket context should expose association state")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        yuan::net::IocpOperation recv_operation;
        std::array<char, 16> recv_buffer{};
        if (!require(accepted_context.begin_operation(recv_operation, yuan::net::IocpOperationKind::recv),
                     "recv operation should begin through socket context")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(accepted_context.pending_operations() == 1,
                     "recv begin should increment pending count")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(yuan::net::IocpTcpIo::post_recv(accepted_context.fd(),
                                                      recv_buffer.data(),
                                                      static_cast<uint32_t>(recv_buffer.size()),
                                                      recv_operation.native_overlapped()),
                     "overlapped recv should post")) {
            accepted_context.complete_operation(recv_operation);
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        constexpr char kPayload[] = "ping";
        if (!require(::send(static_cast<SOCKET>(client), kPayload, 4, 0) == 4,
                     "client send should succeed")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(port.wait(1000, completion), "recv completion should arrive")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(accepted_context.complete_operation(recv_operation),
                     "recv completion should belong to socket context")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(completion.ok, "recv completion should be ok")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(completion.operation == recv_operation.native_overlapped(),
                     "recv operation should round-trip")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(completion.bytes == 4 && std::memcmp(recv_buffer.data(), kPayload, 4) == 0,
                     "recv payload should round-trip")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(accepted_context.pending_operations() == 0,
                     "recv completion should decrement pending count")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        yuan::net::IocpOperation send_operation;
        constexpr char kReply[] = "pong";
        if (!require(accepted_context.begin_operation(send_operation, yuan::net::IocpOperationKind::send),
                     "send operation should begin through socket context")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(yuan::net::IocpTcpIo::post_send(accepted_context.fd(),
                                                     kReply,
                                                     4,
                                                     send_operation.native_overlapped()),
                     "overlapped send should post")) {
            accepted_context.complete_operation(send_operation);
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(port.wait(1000, completion), "send completion should arrive")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(accepted_context.complete_operation(send_operation),
                     "send completion should belong to socket context")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(completion.ok, "send completion should be ok")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(completion.operation == send_operation.native_overlapped(),
                     "send operation should round-trip")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        std::array<char, 16> reply_buffer{};
        if (!require(::recv(static_cast<SOCKET>(client), reply_buffer.data(), 4, 0) == 4,
                     "client recv should succeed")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(std::memcmp(reply_buffer.data(), kReply, 4) == 0,
                     "send payload should round-trip")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(yuan::net::IocpTcpIo::cancel(accepted),
                     "cancel with no outstanding accepted socket IO should be harmless")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(accepted_context.request_close(true),
                     "socket context close request without pending IO should succeed")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(accepted_context.closing(), "socket context should report closing")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        yuan::net::IocpOperation blocked_operation;
        if (!require(!accepted_context.begin_operation(blocked_operation, yuan::net::IocpOperationKind::recv),
                     "closing socket context should reject new operations")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        yuan::net::socket::close_fd(client);
        yuan::net::socket::close_fd(accepted);
        yuan::net::socket::close_fd(listener);
        return true;
    }

    bool test_connect_ex_round_trip()
    {
        yuan::net::IocpCompletionPort port;
        if (!require(port.init(), "connect iocp init should succeed")) {
            return false;
        }

        const int listener = yuan::net::socket::create_ipv4_overlapped_tcp_socket(false);
        if (!require(listener >= 0, "connect listener socket should be created")) {
            return false;
        }
        yuan::net::socket::set_reuse_addr(listener, true);
        if (!require(yuan::net::socket::bind(listener, yuan::net::InetAddress("127.0.0.1", 0)) == 0,
                     "connect listener bind should succeed")) {
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(yuan::net::socket::listen(listener, 16) == 0,
                     "connect listener listen should succeed")) {
            yuan::net::socket::close_fd(listener);
            return false;
        }

        const auto listen_addr = yuan::net::socket::get_local_address(listener);
        int accepted = -1;
        std::thread accept_thread([&]() {
            sockaddr_storage peer{};
            accepted = yuan::net::socket::accept(listener, peer);
        });

        const int client = yuan::net::socket::create_ipv4_overlapped_tcp_socket(false);
        if (!require(client >= 0, "connect client socket should be created")) {
            yuan::net::socket::close_fd(listener);
            if (accept_thread.joinable()) {
                accept_thread.join();
            }
            return false;
        }
        if (!require(yuan::net::socket::bind(client, yuan::net::InetAddress("0.0.0.0", 0)) == 0,
                     "ConnectEx client must bind local address")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(listener);
            if (accept_thread.joinable()) {
                accept_thread.join();
            }
            return false;
        }

        yuan::net::IocpSocketContext client_context;
        if (!require(client_context.attach(client, port, 0xc001),
                     "connect client context should associate with iocp")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(listener);
            if (accept_thread.joinable()) {
                accept_thread.join();
            }
            return false;
        }

        yuan::net::IocpConnectEx connect_ex;
        if (!require(connect_ex.load(client), "ConnectEx function should load")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(listener);
            if (accept_thread.joinable()) {
                accept_thread.join();
            }
            return false;
        }

        yuan::net::IocpOperation connect_operation;
        if (!require(client_context.begin_operation(connect_operation, yuan::net::IocpOperationKind::connect),
                     "connect operation should begin through socket context")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(listener);
            if (accept_thread.joinable()) {
                accept_thread.join();
            }
            return false;
        }

        const auto remote = listen_addr.to_sockaddr();
        const int remote_len = listen_addr.is_ipv6()
            ? static_cast<int>(sizeof(sockaddr_in6))
            : static_cast<int>(sizeof(sockaddr_in));
        if (!require(connect_ex.post(client_context.fd(),
                                     reinterpret_cast<const sockaddr *>(&remote),
                                     remote_len,
                                     connect_operation.native_overlapped()),
                     "ConnectEx post should succeed or pend")) {
            client_context.complete_operation(connect_operation);
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(listener);
            if (accept_thread.joinable()) {
                accept_thread.join();
            }
            return false;
        }

        yuan::net::IocpCompletion completion;
        if (!require(port.wait(1000, completion), "ConnectEx completion should arrive")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(listener);
            if (accept_thread.joinable()) {
                accept_thread.join();
            }
            return false;
        }
        if (!require(client_context.complete_operation(connect_operation),
                     "connect completion should belong to client context")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(listener);
            if (accept_thread.joinable()) {
                accept_thread.join();
            }
            return false;
        }
        if (!require(completion.ok, "ConnectEx completion should be ok")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(listener);
            if (accept_thread.joinable()) {
                accept_thread.join();
            }
            return false;
        }
        if (!require(completion.operation == connect_operation.native_overlapped(),
                     "ConnectEx operation should round-trip")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(listener);
            if (accept_thread.joinable()) {
                accept_thread.join();
            }
            return false;
        }
        if (!require(connect_ex.update_connect_context(client),
                     "ConnectEx socket context should update")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(listener);
            if (accept_thread.joinable()) {
                accept_thread.join();
            }
            return false;
        }

        if (accept_thread.joinable()) {
            accept_thread.join();
        }
        if (!require(accepted >= 0, "listener should accept ConnectEx client")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        yuan::net::socket::close_fd(accepted);
        yuan::net::socket::close_fd(client);
        yuan::net::socket::close_fd(listener);
        return true;
    }

    bool test_tcp_engine_connect_loopback()
    {
        const int listener = yuan::net::socket::create_ipv4_tcp_socket(false);
        if (!require(listener >= 0, "tcp engine connect listener socket should be created")) {
            return false;
        }
        yuan::net::socket::set_reuse_addr(listener, true);
        if (!require(yuan::net::socket::bind(listener, yuan::net::InetAddress("127.0.0.1", 0)) == 0,
                     "tcp engine connect listener bind should succeed")) {
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(yuan::net::socket::listen(listener, 16) == 0,
                     "tcp engine connect listener listen should succeed")) {
            yuan::net::socket::close_fd(listener);
            return false;
        }

        const auto listen_addr = yuan::net::socket::get_local_address(listener);
        std::atomic_bool server_done{false};
        std::thread server_thread([&]() {
            sockaddr_storage peer{};
            const int accepted = yuan::net::socket::accept(listener, peer);
            if (accepted < 0) {
                return;
            }
            std::array<char, 16> request{};
            const int received = ::recv(static_cast<SOCKET>(accepted), request.data(), 4, 0);
            if (received == 4 && std::memcmp(request.data(), "ping", 4) == 0) {
                (void)::send(static_cast<SOCKET>(accepted), "pong", 4, 0);
                server_done.store(true, std::memory_order_release);
            }
            yuan::net::socket::close_fd(accepted);
        });

        yuan::net::IocpTcpEngine client;
        std::mutex mutex;
        std::condition_variable cond;
        std::atomic_bool connected{false};
        std::atomic_bool read_seen{false};

        yuan::net::IocpTcpEngineCallbacks callbacks;
        callbacks.on_connect = [&](const std::shared_ptr<yuan::net::IocpTcpConnection> &connection) {
            connected.store(true, std::memory_order_release);
            connection->send("ping", 4);
            cond.notify_all();
        };
        callbacks.on_read = [&](const std::shared_ptr<yuan::net::IocpTcpConnection> &,
                                const char *data,
                                std::size_t size) {
            if (size == 4 && std::memcmp(data, "pong", 4) == 0) {
                read_seen.store(true, std::memory_order_release);
                cond.notify_all();
            }
        };

        if (!require(client.connect("127.0.0.1",
                                    static_cast<uint16_t>(listen_addr.get_port()),
                                    1,
                                    std::move(callbacks)),
                     "tcp engine ConnectEx client should start")) {
            yuan::net::socket::close_fd(listener);
            if (server_thread.joinable()) {
                server_thread.join();
            }
            return false;
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!cond.wait_for(lock, std::chrono::seconds(2), [&]() {
                    return connected.load(std::memory_order_acquire) &&
                           read_seen.load(std::memory_order_acquire);
                })) {
                client.stop();
                yuan::net::socket::close_fd(listener);
                if (server_thread.joinable()) {
                    server_thread.join();
                }
                return require(false, "tcp engine ConnectEx callbacks should round-trip");
            }
        }

        client.stop();
        yuan::net::socket::close_fd(listener);
        if (server_thread.joinable()) {
            server_thread.join();
        }
        return require(server_done.load(std::memory_order_acquire),
                       "tcp engine ConnectEx server should observe request");
    }

    bool test_tcp_engine_loopback()
    {
        yuan::net::IocpTcpEngine engine;
        std::mutex mutex;
        std::condition_variable cond;
        std::atomic_bool accepted{false};
        std::atomic_bool read_seen{false};
        auto handler = std::make_shared<RecordingConnectionHandler>();

        yuan::net::IocpTcpEngineCallbacks callbacks;
        callbacks.on_accept = [&](const std::shared_ptr<yuan::net::IocpTcpConnection> &connection) {
            connection->set_connection_handler(handler);
            accepted.store(true, std::memory_order_release);
            cond.notify_all();
        };
        callbacks.on_read = [&](const std::shared_ptr<yuan::net::IocpTcpConnection> &connection,
                                const char *data,
                                std::size_t size) {
            if (size == 5 && std::memcmp(data, "hello", 5) == 0) {
                read_seen.store(true, std::memory_order_release);
                connection->send("world", 5);
                cond.notify_all();
            }
        };
        callbacks.on_close = [&](const std::shared_ptr<yuan::net::IocpTcpConnection> &) {
            cond.notify_all();
        };

        if (!require(engine.listen("127.0.0.1", 0, 2, std::move(callbacks)),
                     "tcp engine listen should succeed")) {
            return false;
        }

        const int client = yuan::net::socket::create_ipv4_tcp_socket(false);
        if (!require(client >= 0, "tcp engine client socket should be created")) {
            engine.stop();
            return false;
        }
        if (!require(yuan::net::socket::connect(client, yuan::net::InetAddress("127.0.0.1", engine.local_port())) == 0,
                     "tcp engine client connect should succeed")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }
        if (!require(::send(static_cast<SOCKET>(client), "hello", 5, 0) == 5,
                     "tcp engine client send should succeed")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }

        std::array<char, 16> reply{};
        if (!require(::recv(static_cast<SOCKET>(client), reply.data(), 5, 0) == 5,
                     "tcp engine client recv should succeed")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }
        if (!require(std::memcmp(reply.data(), "world", 5) == 0,
                     "tcp engine reply should round-trip")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }
        if (!require(handler->connected.load(std::memory_order_acquire),
                     "tcp engine connection handler should observe connected")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }
        if (!require(handler->readable.load(std::memory_order_acquire),
                     "tcp engine connection handler should observe read")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!cond.wait_for(lock, std::chrono::seconds(1), [&]() {
                return accepted.load(std::memory_order_acquire) &&
                       read_seen.load(std::memory_order_acquire) &&
                       handler->writable.load(std::memory_order_acquire);
                })) {
                yuan::net::socket::close_fd(client);
                engine.stop();
                return require(false, "tcp engine callbacks and write handler should run");
            }
        }

        yuan::net::socket::close_fd(client);
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!cond.wait_for(lock, std::chrono::seconds(1), [&]() {
                    return handler->closed.load(std::memory_order_acquire) &&
                           handler->closed_state.load(std::memory_order_acquire);
                })) {
                engine.stop();
                return require(false, "tcp engine close handler should observe closed state");
            }
        }
        engine.stop();
        if (!require(!engine.running(), "tcp engine should stop")) {
            return false;
        }
        return true;
    }

    bool test_tcp_engine_ipv6_loopback()
    {
        yuan::net::IocpTcpEngine engine;
        std::mutex mutex;
        std::condition_variable cond;
        std::atomic_bool read_seen{false};

        yuan::net::IocpTcpEngineCallbacks callbacks;
        callbacks.on_read = [&](const std::shared_ptr<yuan::net::IocpTcpConnection> &connection,
                                const char *data,
                                std::size_t size) {
            if (size == 4 && std::memcmp(data, "ping", 4) == 0) {
                read_seen.store(true, std::memory_order_release);
                connection->send("pong", 4);
                cond.notify_all();
            }
        };

        if (!require(engine.listen("::1", 0, 1, std::move(callbacks)),
                     "tcp engine IPv6 listen should succeed")) {
            return false;
        }

        const int client = yuan::net::socket::create_ipv6_tcp_socket(false);
        if (!require(client >= 0, "tcp engine IPv6 client socket should be created")) {
            engine.stop();
            return false;
        }
        if (!require(yuan::net::socket::connect(client, yuan::net::InetAddress("::1", engine.local_port())) == 0,
                     "tcp engine IPv6 client connect should succeed")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }
        if (!require(::send(static_cast<SOCKET>(client), "ping", 4, 0) == 4,
                     "tcp engine IPv6 client send should succeed")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }

        std::array<char, 16> reply{};
        if (!require(::recv(static_cast<SOCKET>(client), reply.data(), 4, 0) == 4,
                     "tcp engine IPv6 client recv should succeed")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }
        if (!require(std::memcmp(reply.data(), "pong", 4) == 0,
                     "tcp engine IPv6 reply should round-trip")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!cond.wait_for(lock, std::chrono::seconds(1), [&]() {
                    return read_seen.load(std::memory_order_acquire);
                })) {
                yuan::net::socket::close_fd(client);
                engine.stop();
                return require(false, "tcp engine IPv6 callbacks should run");
            }
        }

        yuan::net::socket::close_fd(client);
        engine.stop();
        return true;
    }

    bool test_tcp_engine_close_drains_output()
    {
        yuan::net::IocpTcpEngine engine;
        yuan::net::IocpTcpEngineCallbacks callbacks;
        callbacks.on_read = [&](const std::shared_ptr<yuan::net::IocpTcpConnection> &connection,
                                const char *data,
                                std::size_t size) {
            if (size == 5 && std::memcmp(data, "close", 5) == 0) {
                yuan::buffer::ByteBuffer reply;
                reply.append("drain", 5);
                connection->write_and_flush(reply);
                connection->close();
            }
        };

        if (!require(engine.listen("127.0.0.1", 0, 1, std::move(callbacks)),
                     "tcp engine close-drain listen should succeed")) {
            return false;
        }

        const int client = yuan::net::socket::create_ipv4_tcp_socket(false);
        if (!require(client >= 0, "tcp engine close-drain client socket should be created")) {
            engine.stop();
            return false;
        }
        if (!require(yuan::net::socket::connect(client, yuan::net::InetAddress("127.0.0.1", engine.local_port())) == 0,
                     "tcp engine close-drain client connect should succeed")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }
        if (!require(::send(static_cast<SOCKET>(client), "close", 5, 0) == 5,
                     "tcp engine close-drain client send should succeed")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }

        std::array<char, 16> reply{};
        if (!require(::recv(static_cast<SOCKET>(client), reply.data(), 5, 0) == 5,
                     "tcp engine close should drain output before closing")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }
        const bool ok = std::memcmp(reply.data(), "drain", 5) == 0;
        yuan::net::socket::close_fd(client);
        engine.stop();
        return require(ok, "tcp engine close-drain reply should round-trip");
    }

    bool test_tcp_engine_peer_half_close_drains_response()
    {
        yuan::net::IocpTcpEngine engine;
        std::mutex mutex;
        std::condition_variable cond;
        std::atomic_bool saw_input_shutdown{false};
        std::atomic_bool saw_close{false};

        class HalfCloseHandler final : public yuan::net::ConnectionHandler
        {
        public:
            HalfCloseHandler(std::atomic_bool &input_shutdown, std::condition_variable &cv)
                : input_shutdown_(input_shutdown), cv_(cv)
            {
            }

            void on_connected(yuan::net::Connection &) override
            {
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

            void on_input_shutdown(yuan::net::Connection &conn) override
            {
                if (conn.input_shutdown()) {
                    input_shutdown_.store(true, std::memory_order_release);
                    cv_.notify_all();
                }
            }

        private:
            std::atomic_bool &input_shutdown_;
            std::condition_variable &cv_;
        };

        auto handler = std::make_shared<HalfCloseHandler>(saw_input_shutdown, cond);

        yuan::net::IocpTcpEngineCallbacks callbacks;
        callbacks.on_accept = [&](const std::shared_ptr<yuan::net::IocpTcpConnection> &connection) {
            connection->set_connection_handler(handler);
        };
        callbacks.on_read = [&](const std::shared_ptr<yuan::net::IocpTcpConnection> &connection,
                                const char *data,
                                std::size_t size) {
            if (size == 5 && std::memcmp(data, "half!", 5) == 0) {
                yuan::buffer::ByteBuffer reply;
                reply.append("reply", 5);
                connection->write_and_flush(reply);
            }
        };
        callbacks.on_close = [&](const std::shared_ptr<yuan::net::IocpTcpConnection> &) {
            saw_close.store(true, std::memory_order_release);
            cond.notify_all();
        };

        if (!require(engine.listen("127.0.0.1", 0, 1, std::move(callbacks)),
                     "tcp engine half-close listen should succeed")) {
            return false;
        }

        const int client = yuan::net::socket::create_ipv4_tcp_socket(false);
        if (!require(client >= 0, "tcp engine half-close client socket should be created")) {
            engine.stop();
            return false;
        }
        set_recv_timeout(client, 2000);
        if (!require(yuan::net::socket::connect(client, yuan::net::InetAddress("127.0.0.1", engine.local_port())) == 0,
                     "tcp engine half-close client connect should succeed")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }
        if (!require(::send(static_cast<SOCKET>(client), "half!", 5, 0) == 5,
                     "tcp engine half-close client send should succeed")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }
        shutdown_send(client);

        std::array<char, 16> reply{};
        if (!require(::recv(static_cast<SOCKET>(client), reply.data(), 5, 0) == 5,
                     "tcp engine half-close should receive response before close")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }
        if (!require(std::memcmp(reply.data(), "reply", 5) == 0,
                     "tcp engine half-close response should match")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }

        std::array<char, 1> eof{};
        if (!require(::recv(static_cast<SOCKET>(client), eof.data(), 1, 0) == 0,
                     "tcp engine half-close should close after drained response")) {
            yuan::net::socket::close_fd(client);
            engine.stop();
            return false;
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!cond.wait_for(lock, std::chrono::seconds(1), [&]() {
                    return saw_input_shutdown.load(std::memory_order_acquire) &&
                           saw_close.load(std::memory_order_acquire);
                })) {
                yuan::net::socket::close_fd(client);
                engine.stop();
                return require(false, "tcp engine half-close should notify input shutdown and close");
            }
        }

        yuan::net::socket::close_fd(client);
        engine.stop();
        return true;
    }

    bool test_iocp_connection_read_dispatch_gate()
    {
        yuan::net::IocpTcpEngine engine;
        auto connection = std::make_shared<yuan::net::IocpTcpConnection>(
            engine,
            -1,
            yuan::net::InetAddress("127.0.0.1", 0),
            yuan::net::InetAddress("127.0.0.1", 1));

        if (!require(connection->try_mark_read_dispatch_pending(),
                     "iocp read dispatch gate should allow first marker")) {
            return false;
        }
        if (!require(!connection->try_mark_read_dispatch_pending(),
                     "iocp read dispatch gate should reject duplicate marker")) {
            return false;
        }

        connection->clear_read_dispatch_pending();
        if (!require(connection->try_mark_read_dispatch_pending(),
                     "iocp read dispatch gate should allow marker after clear")) {
            return false;
        }
        connection->clear_read_dispatch_pending();
        return true;
    }

    bool test_iocp_async_listener_host_loopback()
    {
        const auto port = reserve_tcp_port();
        if (!require(port != 0, "iocp async listener should reserve a port")) {
            return false;
        }

        yuan::net::NetworkRuntime runtime;
        yuan::net::AsyncListenerHost listener;
        yuan::net::ListenOptions options;
        options.use_iocp = true;
        options.iocp_worker_count = 1;

        std::atomic_bool handled{false};
        std::atomic_bool runtime_done{false};

        listener.set_connection_handler([&](yuan::net::AsyncConnectionContext ctx)->yuan::coroutine::Task<void> {
            auto read = co_await ctx.read_async(1000);
            if (read.status == yuan::coroutine::IoStatus::success &&
                read.data.readable_bytes() == 5 &&
                std::memcmp(read.data.read_ptr(), "hello", 5) == 0) {
                yuan::buffer::ByteBuffer reply;
                reply.append("world", 5);
                auto write = co_await ctx.write_async(reply, 1000);
                if (write.status != yuan::coroutine::IoStatus::success) {
                    runtime.stop();
                    co_return;
                }
            } else {
                runtime.stop();
                co_return;
            }

            auto second_read = co_await ctx.read_async(1000);
            if (second_read.status == yuan::coroutine::IoStatus::success &&
                second_read.data.readable_bytes() == 5 &&
                std::memcmp(second_read.data.read_ptr(), "again", 5) == 0) {
                ctx.append_output("flush");
                auto flush = co_await ctx.flush_async(1000);
                handled.store(flush.status == yuan::coroutine::IoStatus::success, std::memory_order_release);
            }
            co_return;
        });

        if (!require(listener.bind("127.0.0.1", port, runtime, options),
                     "iocp async listener should bind")) {
            return false;
        }

        auto accept_task = listener.run_async();
        accept_task.resume();
        accept_task.detach();

        std::thread runtime_thread([&]() {
            runtime.run();
            runtime_done.store(true, std::memory_order_release);
        });

        const int client = yuan::net::socket::create_ipv4_tcp_socket(false);
        if (!require(client >= 0, "iocp async listener client socket should be created")) {
            runtime.stop();
            if (runtime_thread.joinable()) {
                runtime_thread.join();
            }
            listener.close();
            return false;
        }
        if (!require(yuan::net::socket::connect(client, yuan::net::InetAddress("127.0.0.1", port)) == 0,
                     "iocp async listener client connect should succeed")) {
            yuan::net::socket::close_fd(client);
            runtime.stop();
            if (runtime_thread.joinable()) {
                runtime_thread.join();
            }
            listener.close();
            return false;
        }
        if (!require(::send(static_cast<SOCKET>(client), "hello", 5, 0) == 5,
                     "iocp async listener client send should succeed")) {
            yuan::net::socket::close_fd(client);
            runtime.stop();
            if (runtime_thread.joinable()) {
                runtime_thread.join();
            }
            listener.close();
            return false;
        }

        std::array<char, 16> reply{};
        if (!require(::recv(static_cast<SOCKET>(client), reply.data(), 5, 0) == 5,
                     "iocp async listener client recv should succeed")) {
            yuan::net::socket::close_fd(client);
            runtime.stop();
            if (runtime_thread.joinable()) {
                runtime_thread.join();
            }
            listener.close();
            return false;
        }
        if (!require(std::memcmp(reply.data(), "world", 5) == 0,
                     "iocp async listener reply should round-trip")) {
            yuan::net::socket::close_fd(client);
            runtime.stop();
            if (runtime_thread.joinable()) {
                runtime_thread.join();
            }
            listener.close();
            return false;
        }
        if (!require(::send(static_cast<SOCKET>(client), "again", 5, 0) == 5,
                     "iocp async listener second client send should succeed")) {
            yuan::net::socket::close_fd(client);
            runtime.stop();
            if (runtime_thread.joinable()) {
                runtime_thread.join();
            }
            listener.close();
            return false;
        }
        std::array<char, 16> second_reply{};
        if (!require(::recv(static_cast<SOCKET>(client), second_reply.data(), 5, 0) == 5,
                     "iocp async listener second client recv should succeed")) {
            yuan::net::socket::close_fd(client);
            runtime.stop();
            if (runtime_thread.joinable()) {
                runtime_thread.join();
            }
            listener.close();
            return false;
        }
        if (!require(std::memcmp(second_reply.data(), "flush", 5) == 0,
                     "iocp async listener flush reply should round-trip")) {
            yuan::net::socket::close_fd(client);
            runtime.stop();
            if (runtime_thread.joinable()) {
                runtime_thread.join();
            }
            listener.close();
            return false;
        }
        yuan::net::socket::close_fd(client);

        for (int i = 0; !runtime_done.load(std::memory_order_acquire) && i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        runtime.stop();
        if (runtime_thread.joinable()) {
            runtime_thread.join();
        }
        listener.close();
        return require(handled.load(std::memory_order_acquire),
                       "iocp async listener coroutine should handle request");
    }

    bool test_iocp_async_listener_half_close_response()
    {
        const auto port = reserve_tcp_port();
        if (!require(port != 0, "iocp async half-close should reserve a port")) {
            return false;
        }

        yuan::net::NetworkRuntime runtime;
        yuan::net::AsyncListenerHost listener;
        yuan::net::ListenOptions options;
        options.use_iocp = true;
        options.iocp_worker_count = 1;

        std::atomic_bool handled{false};
        std::atomic_bool runtime_done{false};

        listener.set_connection_handler([&](yuan::net::AsyncConnectionContext ctx)->yuan::coroutine::Task<void> {
            auto read = co_await ctx.read_async(1000);
            if (read.status == yuan::coroutine::IoStatus::success &&
                read.data.readable_bytes() == 5 &&
                std::memcmp(read.data.read_ptr(), "half!", 5) == 0) {
                yuan::buffer::ByteBuffer reply;
                reply.append("reply", 5);
                ctx.write_and_flush(reply);
                handled.store(true, std::memory_order_release);
            }
            co_return;
        });

        if (!require(listener.bind("127.0.0.1", port, runtime, options),
                     "iocp async half-close listener should bind")) {
            return false;
        }

        auto accept_task = listener.run_async();
        accept_task.resume();
        accept_task.detach();

        std::thread runtime_thread([&]() {
            runtime.run();
            runtime_done.store(true, std::memory_order_release);
        });

        const int client = yuan::net::socket::create_ipv4_tcp_socket(false);
        if (!require(client >= 0, "iocp async half-close client socket should be created")) {
            runtime.stop();
            if (runtime_thread.joinable()) {
                runtime_thread.join();
            }
            listener.close();
            return false;
        }
        set_recv_timeout(client, 2000);
        if (!require(yuan::net::socket::connect(client, yuan::net::InetAddress("127.0.0.1", port)) == 0,
                     "iocp async half-close client connect should succeed")) {
            yuan::net::socket::close_fd(client);
            runtime.stop();
            if (runtime_thread.joinable()) {
                runtime_thread.join();
            }
            listener.close();
            return false;
        }
        if (!require(::send(static_cast<SOCKET>(client), "half!", 5, 0) == 5,
                     "iocp async half-close client send should succeed")) {
            yuan::net::socket::close_fd(client);
            runtime.stop();
            if (runtime_thread.joinable()) {
                runtime_thread.join();
            }
            listener.close();
            return false;
        }
        shutdown_send(client);

        std::array<char, 16> reply{};
        if (!require(::recv(static_cast<SOCKET>(client), reply.data(), 5, 0) == 5,
                     "iocp async half-close should receive response before close")) {
            yuan::net::socket::close_fd(client);
            runtime.stop();
            if (runtime_thread.joinable()) {
                runtime_thread.join();
            }
            listener.close();
            return false;
        }
        if (!require(std::memcmp(reply.data(), "reply", 5) == 0,
                     "iocp async half-close response should match")) {
            yuan::net::socket::close_fd(client);
            runtime.stop();
            if (runtime_thread.joinable()) {
                runtime_thread.join();
            }
            listener.close();
            return false;
        }

        yuan::net::socket::close_fd(client);
        for (int i = 0; !runtime_done.load(std::memory_order_acquire) && i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        runtime.stop();
        if (runtime_thread.joinable()) {
            runtime_thread.join();
        }
        listener.close();
        return require(handled.load(std::memory_order_acquire),
                       "iocp async half-close coroutine should write response");
    }
#endif
}

int main()
{
    yuan::net::IocpCompletionPort port;

#ifdef _WIN32
    if (!require(port.init(), "iocp init should succeed on Windows")) {
        return 1;
    }
    if (!require(port.valid(), "iocp should be valid after init")) {
        return 1;
    }

    int marker = 0;
    constexpr uintptr_t kKey = 0x1234;
    constexpr uint32_t kBytes = 17;
    if (!require(port.post(kKey, &marker, kBytes), "iocp post should succeed")) {
        return 1;
    }

    yuan::net::IocpCompletion completion;
    if (!require(port.wait(1000, completion), "iocp wait should receive posted completion")) {
        return 1;
    }
    if (!require(completion.ok, "posted completion should be ok")) {
        return 1;
    }
    if (!require(completion.key == kKey, "posted completion key should round-trip")) {
        return 1;
    }
    if (!require(completion.bytes == kBytes, "posted completion byte count should round-trip")) {
        return 1;
    }
    if (!require(completion.operation == &marker, "posted completion operation should round-trip")) {
        return 1;
    }

    port.close();
    if (!require(!port.valid(), "iocp should be invalid after close")) {
        return 1;
    }
    if (!test_accept_ex_round_trip()) {
        return 1;
    }
    if (!test_dispatcher_post_and_stop()) {
        return 1;
    }
    if (!test_dispatcher_operation_callback()) {
        return 1;
    }
    if (!test_connect_ex_round_trip()) {
        return 1;
    }
    if (!test_tcp_engine_connect_loopback()) {
        return 1;
    }
    if (!test_tcp_engine_loopback()) {
        return 1;
    }
    if (!test_tcp_engine_ipv6_loopback()) {
        return 1;
    }
    if (!test_tcp_engine_close_drains_output()) {
        return 1;
    }
    if (!test_tcp_engine_peer_half_close_drains_response()) {
        return 1;
    }
    if (!test_iocp_connection_read_dispatch_gate()) {
        return 1;
    }
    if (!test_iocp_async_listener_host_loopback()) {
        return 1;
    }
    if (!test_iocp_async_listener_half_close_response()) {
        return 1;
    }
#else
    if (!require(!port.init(), "iocp init should be unavailable off Windows")) {
        return 1;
    }
    if (!require(!port.valid(), "iocp should remain invalid off Windows")) {
        return 1;
    }
#endif

    return 0;
}
