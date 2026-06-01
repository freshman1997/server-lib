#include "coroutine/io_result.h"
#include "coroutine/task.h"
#include "logger.h"
#include "net/async/async_connection_context.h"
#include "net/async/async_listener_host.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/listen_options.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr std::uint32_t kMagic = 0x59474d45U; // YGME
    constexpr std::size_t kHeaderSize = sizeof(std::uint32_t) + sizeof(std::uint32_t) + sizeof(std::uint64_t);
    constexpr std::array<std::uint32_t, 13> kLatencyBucketsUs{
        50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 250000,
        std::numeric_limits<std::uint32_t>::max()
    };

    void close_socket(socket_t fd)
    {
        if (fd == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        ::closesocket(fd);
#else
        ::close(fd);
#endif
    }

    std::uint64_t now_us()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
    }

    void set_u32(char *data, std::size_t offset, std::uint32_t value)
    {
        std::memcpy(data + offset, &value, sizeof(value));
    }

    void set_u64(char *data, std::size_t offset, std::uint64_t value)
    {
        std::memcpy(data + offset, &value, sizeof(value));
    }

    std::uint32_t get_u32(const char *data, std::size_t offset)
    {
        std::uint32_t value = 0;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    }

    std::uint64_t get_u64(const char *data, std::size_t offset)
    {
        std::uint64_t value = 0;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    }

    bool send_all(socket_t fd, const char *data, std::size_t size)
    {
        std::size_t sent = 0;
        while (sent < size) {
#ifdef _WIN32
            const int rc = ::send(fd, data + sent, static_cast<int>(size - sent), 0);
#else
            const int rc = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
#endif
            if (rc <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(rc);
        }
        return true;
    }

    bool recv_exact(socket_t fd, char *data, std::size_t size)
    {
        std::size_t received = 0;
        while (received < size) {
            const int rc = ::recv(fd, data + received, static_cast<int>(size - received), 0);
            if (rc <= 0) {
                return false;
            }
            received += static_cast<std::size_t>(rc);
        }
        return true;
    }

    void tune_client_socket(socket_t fd)
    {
        int one = 1;
        (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof(one));
    }

    socket_t connect_loopback(std::uint16_t port)
    {
        socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == kInvalidSocket) {
            return kInvalidSocket;
        }
        tune_client_socket(fd);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(fd);
            return kInvalidSocket;
        }
        return fd;
    }

    std::uint16_t reserve_tcp_port()
    {
        socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == kInvalidSocket) {
            return 0;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(fd);
            return 0;
        }
        sockaddr_in bound{};
#ifdef _WIN32
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            close_socket(fd);
            return 0;
        }
        const auto port = ntohs(bound.sin_port);
        close_socket(fd);
        return port;
    }

    struct GameServer
    {
        yuan::net::NetworkRuntime runtime;
        yuan::net::AsyncListenerHost listener;
        std::thread thread;
        std::size_t frame_size = 32;

        yuan::coroutine::Task<void> handle_connection(yuan::net::AsyncConnectionContext ctx)
        {
            std::string input;
            input.reserve(frame_size * 8);
            std::size_t offset = 0;

            while (ctx.is_connected()) {
                auto read = co_await ctx.read_awaiter();
                if (read.status != yuan::coroutine::IoStatus::success) {
                    break;
                }

                const auto span = read.data.readable_span();
                if (span.empty()) {
                    continue;
                }

                input.append(span.data(), span.size());
                bool wrote = false;
                while (input.size() - offset >= frame_size) {
                    const char *frame = input.data() + offset;
                    if (get_u32(frame, 0) != kMagic) {
                        ctx.close();
                        co_return;
                    }
                    ctx.append_output(frame, frame_size);
                    offset += frame_size;
                    wrote = true;
                }
                if (wrote) {
                    ctx.flush();
                }
                if (offset > 0 && (offset == input.size() || offset >= frame_size * 16)) {
                    input.erase(0, offset);
                    offset = 0;
                }
            }

            ctx.close();
            co_return;
        }

        bool start(std::uint16_t port,
                   std::size_t worker_count,
                   std::size_t completion_batch_size,
                   std::size_t payload_size,
                   bool use_iocp,
                   bool use_affinity)
        {
            frame_size = (std::max)(payload_size, kHeaderSize);

            yuan::net::ListenOptions options;
            options.use_iocp = use_iocp;
            options.scheduling_mode = use_affinity
                ? yuan::net::ListenSchedulingMode::affinity
                : yuan::net::ListenSchedulingMode::throughput;
            options.shard_count = worker_count;
            options.iocp_worker_count = worker_count;
            options.iocp_completion_batch_size = completion_batch_size;
            options.backlog = 8192;

            listener.set_connection_handler([this](yuan::net::AsyncConnectionContext ctx) {
                return handle_connection(std::move(ctx));
            });
            if (!listener.bind("127.0.0.1", port, runtime, options)) {
                return false;
            }

            auto task = listener.run_async();
            task.resume();
            task.detach();
            thread = std::thread([this]() { runtime.run(); });
            return true;
        }

        void stop()
        {
            listener.close();
            runtime.stop();
            if (thread.joinable()) {
                thread.join();
            }
        }
    };

    struct ClientStats
    {
        std::uint64_t sent = 0;
        std::uint64_t received = 0;
        std::uint64_t failed = 0;
        std::uint64_t latency_sum_us = 0;
        std::uint32_t max_latency_us = 0;
        std::array<std::uint64_t, kLatencyBucketsUs.size()> buckets{};
    };

    void record_latency(ClientStats &stats, std::uint32_t latency_us)
    {
        stats.latency_sum_us += latency_us;
        stats.max_latency_us = (std::max)(stats.max_latency_us, latency_us);
        for (std::size_t i = 0; i < kLatencyBucketsUs.size(); ++i) {
            if (latency_us <= kLatencyBucketsUs[i]) {
                stats.buckets[i]++;
                return;
            }
        }
    }

    struct ClientConn
    {
        socket_t fd = kInvalidSocket;
        std::vector<char> frame;
        std::vector<char> echoed;
        Clock::time_point next_send{};
        std::uint32_t sequence = 0;
    };

    bool initialize_client(ClientConn &conn, std::uint16_t port, std::size_t frame_size)
    {
        conn.fd = connect_loopback(port);
        if (conn.fd == kInvalidSocket) {
            return false;
        }
        conn.frame.assign(frame_size, 0);
        conn.echoed.assign(frame_size, 0);
        conn.next_send = Clock::now();
        conn.sequence = 0;
        set_u32(conn.frame.data(), 0, kMagic);
        return true;
    }

    ClientStats run_client_group(std::uint16_t port,
                                 std::size_t begin,
                                 std::size_t end,
                                 std::chrono::seconds duration,
                                 std::size_t packets_per_second,
                                 std::size_t frame_size)
    {
        ClientStats stats;
        std::vector<ClientConn> clients(end - begin);
        for (auto &client : clients) {
            if (!initialize_client(client, port, frame_size)) {
                stats.failed++;
            }
        }

        const auto interval = packets_per_second == 0
            ? std::chrono::microseconds{0}
            : std::chrono::microseconds{static_cast<long long>((std::max<std::size_t>)(1, 1000000ULL / packets_per_second))};
        const auto deadline = Clock::now() + duration;

        while (Clock::now() < deadline) {
            bool did_work = false;
            for (auto &client : clients) {
                if (client.fd == kInvalidSocket) {
                    continue;
                }
                const auto now = Clock::now();
                if (interval.count() > 0 && now < client.next_send) {
                    continue;
                }

                set_u32(client.frame.data(), sizeof(std::uint32_t), client.sequence++);
                set_u64(client.frame.data(), sizeof(std::uint32_t) * 2, now_us());
                if (!send_all(client.fd, client.frame.data(), client.frame.size())) {
                    stats.failed++;
                    close_socket(client.fd);
                    client.fd = kInvalidSocket;
                    continue;
                }
                stats.sent++;

                if (!recv_exact(client.fd, client.echoed.data(), client.echoed.size())) {
                    stats.failed++;
                    close_socket(client.fd);
                    client.fd = kInvalidSocket;
                    continue;
                }
                stats.received++;
                const auto sent_at = get_u64(client.echoed.data(), sizeof(std::uint32_t) * 2);
                const auto elapsed = now_us() - sent_at;
                record_latency(stats, static_cast<std::uint32_t>((std::min<std::uint64_t>)(elapsed, UINT32_MAX)));

                did_work = true;
                if (interval.count() > 0) {
                    client.next_send += interval;
                    if (client.next_send < Clock::now() - interval) {
                        client.next_send = Clock::now() + interval;
                    }
                }
            }

            if (!did_work && interval.count() > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }

        for (auto &client : clients) {
            close_socket(client.fd);
        }
        return stats;
    }

    std::uint32_t percentile_from_buckets(const ClientStats &stats, double p)
    {
        if (stats.received == 0) {
            return 0;
        }
        const auto target = static_cast<std::uint64_t>(
            (std::max<double>)(1.0, std::ceil(static_cast<double>(stats.received) * p)));
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < stats.buckets.size(); ++i) {
            cumulative += stats.buckets[i];
            if (cumulative >= target) {
                return kLatencyBucketsUs[i];
            }
        }
        return kLatencyBucketsUs.back();
    }

    void merge_stats(ClientStats &dst, const ClientStats &src)
    {
        dst.sent += src.sent;
        dst.received += src.received;
        dst.failed += src.failed;
        dst.latency_sum_us += src.latency_sum_us;
        dst.max_latency_us = (std::max)(dst.max_latency_us, src.max_latency_us);
        for (std::size_t i = 0; i < dst.buckets.size(); ++i) {
            dst.buckets[i] += src.buckets[i];
        }
    }
}

int main(int argc, char **argv)
{
    LOG_GET_REGISTRY()->set_global_level(yuan::log::Level::fatal);
    LOG_GET_REGISTRY()->disable_file_log();

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    const auto hardware = (std::max)(1u, std::thread::hardware_concurrency());
    const auto worker_count = argc > 1 ? static_cast<std::size_t>(std::stoul(argv[1]))
                                       : static_cast<std::size_t>((std::min)(hardware, 4u));
    const auto duration = argc > 2 ? std::chrono::seconds(std::stoul(argv[2])) : std::chrono::seconds(10);
    const auto connections = argc > 3 ? static_cast<std::size_t>(std::stoul(argv[3])) : std::size_t{512};
    const auto packets_per_second = argc > 4 ? static_cast<std::size_t>(std::stoul(argv[4])) : std::size_t{20};
    const auto completion_batch_size = argc > 5 ? static_cast<std::size_t>(std::stoul(argv[5])) : std::size_t{1};
    const auto frame_size = argc > 6 ? static_cast<std::size_t>(std::stoul(argv[6])) : std::size_t{32};
    const auto client_threads = argc > 7
        ? static_cast<std::size_t>(std::stoul(argv[7]))
        : static_cast<std::size_t>((std::min<std::size_t>)((std::max)(1u, hardware * 2), (std::max<std::size_t>)(1, connections)));
#ifdef _WIN32
    const std::string backend = argc > 8 ? argv[8] : "iocp";
#else
    const std::string backend = argc > 8 ? argv[8] : "epoll";
#endif

    const bool wants_iocp = backend == "iocp" || backend == "iocp_affinity";
    const bool wants_affinity = backend == "iocp_affinity" ||
                                backend == "epoll_affinity" ||
                                backend == "poller_affinity" ||
                                backend == "affinity";
    const bool wants_epoll = backend == "epoll" ||
                             backend == "epoll_affinity" ||
                             backend == "poller" ||
                             backend == "poller_affinity" ||
                             backend == "affinity";

    if (!wants_iocp && !wants_epoll) {
        std::cerr << "unknown backend: " << backend
                  << " (supported: iocp, iocp_affinity, epoll, epoll_affinity, poller, poller_affinity, affinity)\n";
        return 1;
    }

#ifdef _WIN32
    const bool use_iocp = wants_iocp;
#else
    if (wants_iocp) {
        std::cerr << "backend '" << backend
                  << "' is not available on this platform; falling back to epoll"
                  << (wants_affinity ? "_affinity" : "") << "\n";
    }
    const bool use_iocp = false;
#endif
    const bool use_affinity = wants_affinity;

    if (connections == 0 || client_threads == 0) {
        std::cerr << "connections and client_threads must be greater than zero\n";
        return 1;
    }

    std::uint16_t port = 0;
    GameServer server;
    for (int attempt = 0; attempt < 8; ++attempt) {
        port = reserve_tcp_port();
        if (port != 0 && server.start(port,
                                      worker_count,
                                      completion_batch_size,
                                      frame_size,
                                      use_iocp,
                                      use_affinity)) {
            break;
        }
        server.stop();
        port = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (port == 0) {
        std::cerr << "failed to start game benchmark server\n";
        return 1;
    }

    const auto effective_frame_size = (std::max)(frame_size, kHeaderSize);
    const auto effective_client_threads = (std::min)(client_threads, connections);
    const char *backend_label = use_iocp
        ? (use_affinity ? "iocp_affinity" : "iocp")
        : (use_affinity ? "epoll_affinity" : "epoll");
    std::cout << "game server benchmark backend=" << backend_label
              << " workers=" << worker_count
              << " duration_s=" << duration.count()
              << " connections=" << connections
              << " pps_per_connection=" << packets_per_second
              << " completion_batch_size=" << completion_batch_size
              << " frame_size=" << effective_frame_size
              << " client_threads=" << effective_client_threads << '\n';

    std::vector<std::thread> threads;
    std::vector<ClientStats> per_thread(effective_client_threads);
    threads.reserve(effective_client_threads);
    const auto started = Clock::now();
    for (std::size_t i = 0; i < effective_client_threads; ++i) {
        const auto begin = i * connections / effective_client_threads;
        const auto end = (i + 1) * connections / effective_client_threads;
        threads.emplace_back([&, i, begin, end]() {
            per_thread[i] = run_client_group(port, begin, end, duration, packets_per_second, effective_frame_size);
        });
    }
    for (auto &thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();

    ClientStats total;
    for (const auto &stats : per_thread) {
        merge_stats(total, stats);
    }

    const auto avg_us = total.received == 0
        ? 0.0
        : static_cast<double>(total.latency_sum_us) / static_cast<double>(total.received);
    std::cout << "sent=" << total.sent
              << " received=" << total.received
              << " failed=" << total.failed
              << " packets_per_second=" << (elapsed > 0.0 ? static_cast<double>(total.received) / elapsed : 0.0)
              << " avg_rtt_us=" << avg_us
              << " p50_rtt_us<=" << percentile_from_buckets(total, 0.50)
              << " p95_rtt_us<=" << percentile_from_buckets(total, 0.95)
              << " p99_rtt_us<=" << percentile_from_buckets(total, 0.99)
              << " max_rtt_us=" << total.max_latency_us
              << '\n';

    server.stop();
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
