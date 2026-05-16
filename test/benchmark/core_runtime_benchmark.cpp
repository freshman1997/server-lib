#include "application.h"
#include "buffer/buffer_chain.h"
#include "buffer/byte_buffer.h"
#include "bootstrap.h"
#include "coroutine/io_result.h"
#include "coroutine/task.h"
#include "eventbus/event_bus.h"
#include "net/async/async_connection_context.h"
#include "net/async/async_listener_host.h"
#include "net/connection/connection_factory.h"
#include "net/connection/connection_handle.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/socket_ops.h"
#include "registry.h"

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
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct BenchResult
    {
        std::string name;
        std::uint64_t operations = 0;
        std::uint64_t bytes = 0;
        std::chrono::nanoseconds elapsed{};
    };

    template <typename Fn>
    BenchResult run_bench(std::string name, std::uint64_t operations, std::uint64_t bytes, Fn fn)
    {
        const auto start = Clock::now();
        fn();
        const auto end = Clock::now();
        return BenchResult{std::move(name), operations, bytes,
                           std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)};
    }

    [[noreturn]] void fail(const std::string &message)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }

    void print_result(const BenchResult &result)
    {
        const double seconds = static_cast<double>(result.elapsed.count()) / 1'000'000'000.0;
        const double ops_per_second = seconds > 0.0 ? static_cast<double>(result.operations) / seconds : 0.0;
        const double mib_per_second = seconds > 0.0 && result.bytes > 0
                                          ? (static_cast<double>(result.bytes) / (1024.0 * 1024.0)) / seconds
                                          : 0.0;

        std::cout << std::left << std::setw(30) << result.name
                  << " ops=" << std::setw(12) << result.operations
                  << " elapsed_ms=" << std::setw(10)
                  << (static_cast<double>(result.elapsed.count()) / 1'000'000.0)
                  << " ops_per_s=" << std::setw(14) << ops_per_second;
        if (result.bytes > 0) {
            std::cout << " MiB_per_s=" << mib_per_second;
        }
        std::cout << '\n';
    }

    BenchResult bench_byte_buffer_append_copy()
    {
        constexpr std::uint64_t iterations = 200'000;
        constexpr std::size_t payload_size = 1024;
        const std::string payload(payload_size, 'x');
        std::uint64_t checksum = 0;

        auto result = run_bench("byte_buffer_append_copy",
                                iterations,
                                iterations * payload_size,
                                [&]() {
                                    for (std::uint64_t i = 0; i < iterations; ++i) {
                                        yuan::buffer::ByteBuffer buffer(payload_size);
                                        buffer.append(std::string_view(payload));
                                        auto copy = buffer.copy_readable();
                                        checksum += copy.readable_bytes();
                                    }
                                });

        if (checksum != iterations * payload_size) {
            std::cerr << "byte_buffer_append_copy checksum failed\n";
            std::exit(1);
        }
        return result;
    }

    BenchResult bench_buffer_chain_push_pop()
    {
        constexpr std::uint64_t iterations = 100'000;
        constexpr std::size_t chunks_per_iteration = 8;
        constexpr std::size_t payload_size = 512;
        const std::string payload(payload_size, 'y');
        std::uint64_t checksum = 0;

        auto result = run_bench("buffer_chain_push_pop",
                                iterations * chunks_per_iteration,
                                iterations * chunks_per_iteration * payload_size,
                                [&]() {
                                    for (std::uint64_t i = 0; i < iterations; ++i) {
                                        yuan::buffer::BufferChain chain;
                                        for (std::size_t chunk = 0; chunk < chunks_per_iteration; ++chunk) {
                                            auto *buffer = chain.emplace_back(payload_size);
                                            buffer->append(std::string_view(payload));
                                        }
                                        while (!chain.empty()) {
                                            auto front = chain.pop_front();
                                            checksum += front ? front->readable_bytes() : 0;
                                        }
                                    }
                                });

        if (checksum != iterations * chunks_per_iteration * payload_size) {
            std::cerr << "buffer_chain_push_pop checksum failed\n";
            std::exit(1);
        }
        return result;
    }

    BenchResult bench_event_bus_publish()
    {
        constexpr std::uint64_t iterations = 300'000;
        constexpr int subscribers = 4;
        yuan::eventbus::EventBus bus;
        std::uint64_t checksum = 0;

        for (int i = 0; i < subscribers; ++i) {
            bus.subscribe("core.benchmark", [&checksum](const yuan::eventbus::Event &) {
                ++checksum;
            });
        }

        auto result = run_bench("event_bus_publish",
                                iterations,
                                0,
                                [&]() {
                                    for (std::uint64_t i = 0; i < iterations; ++i) {
                                        bus.publish("core.benchmark");
                                    }
                                });

        if (checksum != iterations * subscribers) {
            std::cerr << "event_bus_publish checksum failed\n";
            std::exit(1);
        }
        return result;
    }

    void close_socket(socket_t sock)
    {
        if (sock == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        ::closesocket(sock);
#else
        ::close(sock);
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

    socket_t connect_loopback(uint16_t port)
    {
        socket_t client = ::socket(AF_INET, SOCK_STREAM, 0);
        if (client == kInvalidSocket) {
            return kInvalidSocket;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (::connect(client, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(client);
            return kInvalidSocket;
        }
        return client;
    }

    bool send_all(socket_t sock, const std::string &data)
    {
        std::size_t sent = 0;
        while (sent < data.size()) {
#ifdef _WIN32
            const int rc = ::send(sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
            const ssize_t rc = ::send(sock, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#endif
            if (rc <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(rc);
        }
        return true;
    }

    std::string recv_exact(socket_t sock, std::size_t expected)
    {
        std::string data;
        data.reserve(expected);
        char buffer[4096];
        while (data.size() < expected) {
#ifdef _WIN32
            const int rc = ::recv(sock, buffer, static_cast<int>((std::min)(sizeof(buffer), expected - data.size())), 0);
#else
            const ssize_t rc = ::recv(sock, buffer, (std::min)(sizeof(buffer), expected - data.size()), 0);
#endif
            if (rc <= 0) {
                break;
            }
            data.append(buffer, static_cast<std::size_t>(rc));
        }
        return data;
    }

    yuan::coroutine::Task<void> coroutine_schedule_chain_task(
        yuan::coroutine::RuntimeView view,
        std::uint64_t iterations,
        std::atomic_uint64_t &checksum)
    {
        for (std::uint64_t i = 0; i < iterations; ++i) {
            co_await view.schedule();
            checksum.fetch_add(1, std::memory_order_relaxed);
        }
        if (auto *loop = view.event_loop()) {
            loop->quit();
        }
    }

    yuan::coroutine::Task<void> detached_coroutine_once(
        yuan::coroutine::RuntimeView view,
        std::atomic_uint64_t &completed,
        std::uint64_t target)
    {
        co_await view.schedule();
        if (completed.fetch_add(1, std::memory_order_acq_rel) + 1 == target) {
            if (auto *loop = view.event_loop()) {
                loop->quit();
            }
        }
    }

    yuan::coroutine::Task<void> timer_coroutine_once(
        yuan::coroutine::RuntimeView view,
        std::atomic_uint64_t &completed,
        std::uint64_t target)
    {
        co_await view.sleep_for(1);
        if (completed.fetch_add(1, std::memory_order_acq_rel) + 1 == target) {
            if (auto *loop = view.event_loop()) {
                loop->quit();
            }
        }
    }

    yuan::coroutine::Task<void> echo_once(yuan::net::AsyncConnectionContext ctx,
                                         std::atomic_uint64_t &handled,
                                         std::atomic_uint64_t &bytes)
    {
        auto read_result = co_await ctx.read_async(5000);
        if (read_result.status == yuan::coroutine::IoStatus::success) {
            bytes.fetch_add(read_result.data.readable_bytes(), std::memory_order_relaxed);
            auto write_result = co_await ctx.write_async(read_result.data, 5000);
            if (write_result.status == yuan::coroutine::IoStatus::success) {
                (void)co_await ctx.flush_async(5000);
            }
        }
        ctx.close();
        handled.fetch_add(1, std::memory_order_relaxed);
    }

    yuan::coroutine::Task<void> echo_stream(yuan::net::AsyncConnectionContext ctx,
                                           std::atomic_uint64_t &messages,
                                           std::atomic_uint64_t &bytes,
                                           std::atomic_uint64_t &closed)
    {
        for (;;) {
            auto read_result = co_await ctx.read_async(5000);
            if (read_result.status != yuan::coroutine::IoStatus::success ||
                read_result.data.readable_bytes() == 0) {
                break;
            }

            bytes.fetch_add(read_result.data.readable_bytes(), std::memory_order_relaxed);
            auto write_result = co_await ctx.write_async(read_result.data, 5000);
            if (write_result.status != yuan::coroutine::IoStatus::success) {
                break;
            }
            (void)co_await ctx.flush_async(5000);
            messages.fetch_add(1, std::memory_order_relaxed);
        }

        ctx.close();
        closed.fetch_add(1, std::memory_order_relaxed);
    }

    class WorkerLifecycleBenchService final
        : public yuan::app::Service,
          public yuan::app::RuntimeContextAwareService
    {
    public:
        void set_runtime_context(const yuan::app::RuntimeContext &context) override
        {
            context_ = context;
        }

        bool init() override
        {
            if (!context_.shared_runtime) {
                return false;
            }
            initialized_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        void start() override
        {
            started_.fetch_add(1, std::memory_order_relaxed);
            context_.shared_runtime->dispatch([]() {
                dispatched_.fetch_add(1, std::memory_order_relaxed);
            });
        }

        void stop() override
        {
            stopped_.fetch_add(1, std::memory_order_relaxed);
        }

        static void reset()
        {
            initialized_.store(0, std::memory_order_release);
            started_.store(0, std::memory_order_release);
            stopped_.store(0, std::memory_order_release);
            dispatched_.store(0, std::memory_order_release);
        }

        static std::uint64_t initialized()
        {
            return initialized_.load(std::memory_order_acquire);
        }

        static std::uint64_t started()
        {
            return started_.load(std::memory_order_acquire);
        }

        static std::uint64_t stopped()
        {
            return stopped_.load(std::memory_order_acquire);
        }

        static std::uint64_t dispatched()
        {
            return dispatched_.load(std::memory_order_acquire);
        }

    private:
        yuan::app::RuntimeContext context_;
        static std::atomic_uint64_t initialized_;
        static std::atomic_uint64_t started_;
        static std::atomic_uint64_t stopped_;
        static std::atomic_uint64_t dispatched_;
    };

    std::atomic_uint64_t WorkerLifecycleBenchService::initialized_{0};
    std::atomic_uint64_t WorkerLifecycleBenchService::started_{0};
    std::atomic_uint64_t WorkerLifecycleBenchService::stopped_{0};
    std::atomic_uint64_t WorkerLifecycleBenchService::dispatched_{0};

    template <typename Predicate>
    bool wait_until(std::chrono::milliseconds timeout, Predicate predicate)
    {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return predicate();
    }

    BenchResult bench_runtime_dispatch_callbacks()
    {
        constexpr std::uint64_t iterations = 300'000;
        yuan::net::NetworkRuntime runtime;
        std::atomic_uint64_t checksum{0};

        auto result = run_bench("runtime_dispatch_callbacks",
                                iterations,
                                0,
                                [&]() {
                                    for (std::uint64_t i = 0; i < iterations; ++i) {
                                        runtime.dispatch([&runtime, &checksum, iterations]() {
                                            const auto current = checksum.fetch_add(1, std::memory_order_acq_rel) + 1;
                                            if (current == iterations) {
                                                runtime.stop();
                                            }
                                        });
                                    }
                                    runtime.run();
                                });

        if (checksum.load(std::memory_order_acquire) != iterations) {
            fail("runtime_dispatch_callbacks checksum failed");
        }
        return result;
    }

    BenchResult bench_coroutine_schedule_chain()
    {
        constexpr std::uint64_t iterations = 150'000;
        yuan::net::NetworkRuntime runtime;
        auto view = runtime.runtime_view();
        std::atomic_uint64_t checksum{0};

        auto result = run_bench("coroutine_schedule_chain",
                                iterations,
                                0,
                                [&]() {
                                    auto task = coroutine_schedule_chain_task(view, iterations, checksum);
                                    task.resume();
                                    runtime.run();
                                    task.get_result();
                                });

        if (checksum.load(std::memory_order_acquire) != iterations) {
            fail("coroutine_schedule_chain checksum failed");
        }
        return result;
    }

    BenchResult bench_detached_coroutine_lifecycle()
    {
        constexpr std::uint64_t iterations = 50'000;
        yuan::net::NetworkRuntime runtime;
        auto view = runtime.runtime_view();
        std::atomic_uint64_t completed{0};

        auto result = run_bench("detached_coroutine_lifecycle",
                                iterations,
                                0,
                                [&]() {
                                    for (std::uint64_t i = 0; i < iterations; ++i) {
                                        auto task = detached_coroutine_once(view, completed, iterations);
                                        task.resume();
                                        task.detach();
                                    }
                                    runtime.run();
                                });

        if (completed.load(std::memory_order_acquire) != iterations) {
            fail("detached_coroutine_lifecycle checksum failed");
        }
        return result;
    }

    BenchResult bench_timer_coroutine_lifecycle()
    {
        constexpr std::uint64_t iterations = 20'000;
        yuan::net::NetworkRuntime runtime;
        auto view = runtime.runtime_view();
        std::atomic_uint64_t completed{0};

        auto result = run_bench("timer_coroutine_lifecycle",
                                iterations,
                                0,
                                [&]() {
                                    for (std::uint64_t i = 0; i < iterations; ++i) {
                                        auto task = timer_coroutine_once(view, completed, iterations);
                                        task.resume();
                                        task.detach();
                                    }
                                    runtime.run();
                                });

        if (completed.load(std::memory_order_acquire) != iterations) {
            fail("timer_coroutine_lifecycle checksum failed");
        }
        return result;
    }

    BenchResult bench_tcp_connection_create_abort()
    {
        constexpr std::uint64_t iterations = 20'000;
        std::uint64_t checksum = 0;

        auto result = run_bench("tcp_connection_create_abort",
                                iterations,
                                0,
                                [&]() {
                                    for (std::uint64_t i = 0; i < iterations; ++i) {
                                        const int fd = yuan::net::socket::create_ipv4_tcp_socket(true);
                                        if (fd < 0) {
                                            fail("tcp_connection_create_abort socket create failed");
                                        }

                                        auto conn = yuan::net::create_stream_connection("127.0.0.1", 0, fd);
                                        if (!conn) {
                                            yuan::net::socket::close_fd(fd);
                                            fail("tcp_connection_create_abort connection create failed");
                                        }

                                        yuan::net::ConnectionHandle handle(conn);
                                        conn->abort();
                                        if (handle) {
                                            ++checksum;
                                        }
                                    }
                                });

        if (checksum != iterations) {
            fail("tcp_connection_create_abort checksum failed");
        }
        return result;
    }

    BenchResult bench_async_listener_persistent_echo_stream()
    {
        constexpr std::uint64_t iterations = 5'000;
        constexpr std::size_t payload_size = 512;
        const std::string payload(payload_size, 's');
        const auto port = reserve_tcp_port();
        if (port == 0) {
            fail("async_listener_persistent_echo reserve port failed");
        }

        yuan::net::NetworkRuntime runtime;
        yuan::net::AsyncListenerHost host;
        std::atomic_uint64_t messages{0};
        std::atomic_uint64_t server_bytes{0};
        std::atomic_uint64_t closed{0};
        std::uint64_t client_bytes = 0;

        host.set_connection_handler([&](yuan::net::AsyncConnectionContext ctx) -> yuan::coroutine::Task<void> {
            co_await echo_stream(std::move(ctx), messages, server_bytes, closed);
        });

        if (!host.bind("127.0.0.1", port, runtime)) {
            fail("async_listener_persistent_echo bind failed");
        }

        auto accept_task = host.run_async();
        accept_task.resume();

        std::thread loop_thread([&runtime]() {
            runtime.run();
        });

        socket_t client = connect_loopback(port);
        if (client == kInvalidSocket) {
            runtime.dispatch([&host, &runtime]() {
                host.close();
                runtime.stop();
            });
            if (loop_thread.joinable()) {
                loop_thread.join();
            }
            fail("async_listener_persistent_echo connect failed");
        }

        auto result = run_bench("async_listener_persistent_echo",
                                iterations,
                                iterations * payload_size * 2,
                                [&]() {
                                    for (std::uint64_t i = 0; i < iterations; ++i) {
                                        if (!send_all(client, payload)) {
                                            fail("async_listener_persistent_echo send failed");
                                        }
                                        auto echoed = recv_exact(client, payload.size());
                                        if (echoed != payload) {
                                            fail("async_listener_persistent_echo echo mismatch");
                                        }
                                        client_bytes += echoed.size();
                                    }
                                });

        close_socket(client);
        if (!wait_until(std::chrono::seconds(5), [&]() {
                return closed.load(std::memory_order_acquire) == 1;
            })) {
            fail("async_listener_persistent_echo close wait failed");
        }

        runtime.dispatch([&host, &runtime]() {
            host.close();
            runtime.stop();
        });
        if (loop_thread.joinable()) {
            loop_thread.join();
        }

        if (messages.load(std::memory_order_acquire) != iterations ||
            server_bytes.load(std::memory_order_acquire) != iterations * payload_size ||
            client_bytes != iterations * payload_size) {
            fail("async_listener_persistent_echo checksum failed");
        }
        return result;
    }

    BenchResult bench_async_listener_concurrent_echo_roundtrip()
    {
        constexpr std::uint64_t iterations = 2'000;
        constexpr std::uint64_t concurrency = 32;
        constexpr std::size_t payload_size = 1024;
        const std::string payload(payload_size, 'q');
        const auto port = reserve_tcp_port();
        if (port == 0) {
            fail("async_listener_concurrent_echo reserve port failed");
        }

        yuan::net::NetworkRuntime runtime;
        yuan::net::AsyncListenerHost host;
        std::atomic_uint64_t handled{0};
        std::atomic_uint64_t server_bytes{0};
        std::atomic_uint64_t client_bytes{0};
        std::atomic_uint64_t failures{0};
        std::atomic_uint64_t next{0};

        host.set_connection_handler([&](yuan::net::AsyncConnectionContext ctx) -> yuan::coroutine::Task<void> {
            co_await echo_once(std::move(ctx), handled, server_bytes);
        });

        if (!host.bind("127.0.0.1", port, runtime)) {
            fail("async_listener_concurrent_echo bind failed");
        }

        auto accept_task = host.run_async();
        accept_task.resume();

        std::thread loop_thread([&runtime]() {
            runtime.run();
        });

        auto result = run_bench("async_listener_concurrent_echo",
                                iterations,
                                iterations * payload_size * 2,
                                [&]() {
                                    std::vector<std::thread> workers;
                                    workers.reserve(concurrency);
                                    for (std::uint64_t worker = 0; worker < concurrency; ++worker) {
                                        workers.emplace_back([&]() {
                                            for (;;) {
                                                const auto index = next.fetch_add(1, std::memory_order_relaxed);
                                                if (index >= iterations) {
                                                    break;
                                                }

                                                socket_t client = connect_loopback(port);
                                                if (client == kInvalidSocket) {
                                                    failures.fetch_add(1, std::memory_order_relaxed);
                                                    continue;
                                                }
                                                if (!send_all(client, payload)) {
                                                    close_socket(client);
                                                    failures.fetch_add(1, std::memory_order_relaxed);
                                                    continue;
                                                }
                                                auto echoed = recv_exact(client, payload.size());
                                                close_socket(client);
                                                if (echoed != payload) {
                                                    failures.fetch_add(1, std::memory_order_relaxed);
                                                    continue;
                                                }
                                                client_bytes.fetch_add(echoed.size(), std::memory_order_relaxed);
                                            }
                                        });
                                    }
                                    for (auto &worker : workers) {
                                        if (worker.joinable()) {
                                            worker.join();
                                        }
                                    }
                                });

        if (!wait_until(std::chrono::seconds(5), [&]() {
                return handled.load(std::memory_order_acquire) == iterations;
            })) {
            fail("async_listener_concurrent_echo handled wait failed");
        }

        runtime.dispatch([&host, &runtime]() {
            host.close();
            runtime.stop();
        });
        if (loop_thread.joinable()) {
            loop_thread.join();
        }

        if (failures.load(std::memory_order_acquire) != 0 ||
            handled.load(std::memory_order_acquire) != iterations ||
            server_bytes.load(std::memory_order_acquire) != iterations * payload_size ||
            client_bytes.load(std::memory_order_acquire) != iterations * payload_size) {
            fail("async_listener_concurrent_echo checksum failed");
        }
        return result;
    }

    BenchResult bench_async_listener_echo_roundtrip()
    {
        constexpr std::uint64_t iterations = 1'000;
        constexpr std::size_t payload_size = 256;
        const std::string payload(payload_size, 'z');
        const auto port = reserve_tcp_port();
        if (port == 0) {
            fail("async_listener_echo_roundtrip reserve port failed");
        }

        yuan::net::NetworkRuntime runtime;
        yuan::net::AsyncListenerHost host;
        std::atomic_uint64_t handled{0};
        std::atomic_uint64_t server_bytes{0};
        std::uint64_t client_bytes = 0;

        host.set_connection_handler([&](yuan::net::AsyncConnectionContext ctx) -> yuan::coroutine::Task<void> {
            co_await echo_once(std::move(ctx), handled, server_bytes);
        });

        if (!host.bind("127.0.0.1", port, runtime)) {
            fail("async_listener_echo_roundtrip bind failed");
        }

        auto accept_task = host.run_async();
        accept_task.resume();

        std::thread loop_thread([&runtime]() {
            runtime.run();
        });

        auto result = run_bench("async_listener_echo_roundtrip",
                                iterations,
                                iterations * payload_size * 2,
                                [&]() {
                                    for (std::uint64_t i = 0; i < iterations; ++i) {
                                        socket_t client = connect_loopback(port);
                                        if (client == kInvalidSocket) {
                                            fail("async_listener_echo_roundtrip connect failed");
                                        }
                                        if (!send_all(client, payload)) {
                                            close_socket(client);
                                            fail("async_listener_echo_roundtrip send failed");
                                        }
                                        auto echoed = recv_exact(client, payload.size());
                                        close_socket(client);
                                        if (echoed != payload) {
                                            fail("async_listener_echo_roundtrip echo mismatch");
                                        }
                                        client_bytes += echoed.size();
                                    }
                                });

        runtime.dispatch([&host, &runtime]() {
            host.close();
            runtime.stop();
        });
        if (loop_thread.joinable()) {
            loop_thread.join();
        }

        if (handled.load(std::memory_order_acquire) != iterations ||
            server_bytes.load(std::memory_order_acquire) != iterations * payload_size ||
            client_bytes != iterations * payload_size) {
            fail("async_listener_echo_roundtrip checksum failed");
        }
        return result;
    }

    BenchResult bench_in_process_worker_lifecycle()
    {
        constexpr std::uint64_t iterations = 50;
        constexpr std::size_t worker_count = 2;
        constexpr auto total_worker_lifecycles = iterations * worker_count;
        WorkerLifecycleBenchService::reset();

        auto result = run_bench("in_process_worker_lifecycle",
                                total_worker_lifecycles,
                                0,
                                [&]() {
                                    for (std::uint64_t i = 0; i < iterations; ++i) {
                                        yuan::app::RuntimeContext context;
                                        context.app_name = "core-runtime-benchmark-worker-lifecycle";
                                        context.run_mode = yuan::app::RunMode::multi_thread;
                                        context.worker_threads = worker_count;
                                        context.runtime_workers.worker_count = worker_count;
                                        context.restart_failed_workers = false;

                                        yuan::app::Application app(context);
                                        yuan::app::ServiceDescriptor descriptor;
                                        descriptor.name = "worker-lifecycle";
                                        descriptor.type_name = "WorkerLifecycleBenchService";
                                        descriptor.contract_id = "benchmark.worker-lifecycle";
                                        descriptor.contract_version = 1;
                                        descriptor.placement.mode = yuan::app::PlacementMode::all_workers;

                                        if (!app.add_service(descriptor, []() {
                                                return std::make_shared<WorkerLifecycleBenchService>();
                                            })) {
                                            fail("in_process_worker_lifecycle service registration failed");
                                        }

                                        const auto expected_dispatches = WorkerLifecycleBenchService::dispatched() + worker_count;
                                        yuan::app::Bootstrap bootstrap(app);
                                        if (!bootstrap.run()) {
                                            fail("in_process_worker_lifecycle bootstrap run failed");
                                        }
                                        if (!wait_until(std::chrono::seconds(2), [&]() {
                                                return WorkerLifecycleBenchService::dispatched() >= expected_dispatches;
                                            })) {
                                            fail("in_process_worker_lifecycle dispatch wait failed");
                                        }
                                        bootstrap.shutdown();
                                    }
                                });

        if (WorkerLifecycleBenchService::initialized() != total_worker_lifecycles ||
            WorkerLifecycleBenchService::started() != total_worker_lifecycles ||
            WorkerLifecycleBenchService::stopped() != total_worker_lifecycles ||
            WorkerLifecycleBenchService::dispatched() != total_worker_lifecycles) {
            fail("in_process_worker_lifecycle checksum failed");
        }
        return result;
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

    std::cout << "core runtime benchmark\n";
    std::cout << "build=manual chrono=steady_clock scope=buffer,event_bus,runtime,coroutine,connection,worker_lifecycle\n";
    yuan::log::LogRegistry::get_instance()->set_global_level(yuan::log::Level::fatal);

    std::vector<BenchResult> results;
    results.push_back(bench_byte_buffer_append_copy());
    results.push_back(bench_buffer_chain_push_pop());
    results.push_back(bench_event_bus_publish());
    results.push_back(bench_runtime_dispatch_callbacks());
    results.push_back(bench_coroutine_schedule_chain());
    results.push_back(bench_detached_coroutine_lifecycle());
    results.push_back(bench_timer_coroutine_lifecycle());
    results.push_back(bench_tcp_connection_create_abort());
    results.push_back(bench_async_listener_echo_roundtrip());
    results.push_back(bench_async_listener_persistent_echo_stream());
    results.push_back(bench_async_listener_concurrent_echo_roundtrip());
    results.push_back(bench_in_process_worker_lifecycle());

    for (const auto &result : results) {
        print_result(result);
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
