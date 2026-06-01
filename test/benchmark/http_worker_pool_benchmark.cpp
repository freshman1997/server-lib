#include "eventbus/event_bus.h"
#include "http/http_service.h"
#include "log.h"
#include "registry.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/iocp/iocp_accept.h"
#include "net/iocp/iocp_completion_port.h"
#include "net/iocp/iocp_dispatcher.h"
#include "net/iocp/iocp_operation.h"
#include "net/iocp/iocp_socket_context.h"
#include "net/iocp/iocp_tcp_engine.h"
#include "net/iocp/iocp_tcp_io.h"
#include "net/async/async_listener_host.h"
#include "net/async/iocp_sharded_async_listener.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/listen_options.h"
#include "response.h"
#include "net/socket/socket_ops.h"
#include "runtime_context.h"
#include "native_platform.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <memory>
#include <array>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

#ifdef YUAN_BENCH_WITH_LIBEVENT
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/thread.h>
#include <event2/util.h>
#endif

#ifdef YUAN_BENCH_WITH_LIBEV
#ifdef EV_READ
#undef EV_READ
#endif
#ifdef EV_WRITE
#undef EV_WRITE
#endif
#ifdef EV_TIMEOUT
#undef EV_TIMEOUT
#endif
#ifdef EV_SIGNAL
#undef EV_SIGNAL
#endif
#ifdef EV_PERSIST
#undef EV_PERSIST
#endif
#ifdef EVLOOP_NONBLOCK
#undef EVLOOP_NONBLOCK
#endif
#include <ev.h>
#endif

#ifdef YUAN_BENCH_WITH_LIBUV
#include <uv.h>
#endif

#ifdef YUAN_BENCH_WITH_WORKFLOW
#include <workflow/WFGlobal.h>
#include <workflow/WFHttpServer.h>
#endif

#ifdef YUAN_BENCH_WITH_LIBHV
#include <HttpServer.h>
#include <HttpService.h>
#endif

#ifdef _WIN32
#include <mswsock.h>
#endif

constexpr const char *kBenchBodyGlobal = "OK";
constexpr const char *kHttpResponseGlobal =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "OK";

#ifdef YUAN_BENCH_WITH_LIBHV
    struct LibhvServer
    {
        std::unique_ptr<hv::HttpServer> server;
        std::unique_ptr<hv::HttpService> service;

        bool start(uint16_t port, std::size_t worker_count)
        {
            service = std::make_unique<hv::HttpService>();
            service->GET("/bench", [](HttpRequest *, HttpResponse *resp) {
                return resp->String(kBenchBodyGlobal);
            });

            server = std::make_unique<hv::HttpServer>();
            server->registerHttpService(service.get());
            server->setHost("127.0.0.1");
            server->setPort(static_cast<int>(port));
            server->setThreadNum(static_cast<int>((std::max)(worker_count, std::size_t{1})));
            return server->start() == 0;
        }

        void stop()
        {
            if (server) {
                server->stop();
                server.reset();
            }
            service.reset();
        }
    };
#endif

namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr const char *kBenchmarkBody = "OK";
    constexpr std::size_t kBenchmarkBodySize = 2;
    constexpr const char *kHttpResponse =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 2\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "OK";

    void close_socket(socket_t fd)
    {
        if (fd == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
    }

    void set_client_socket_options(socket_t fd)
    {
        if (fd == kInvalidSocket) {
            return;
        }

        int flag = 1;
#ifdef _WIN32
        (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&flag), sizeof(flag));
#else
        (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#endif
    }

    void set_nonblocking(socket_t fd)
    {
        if (fd == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        u_long mode = 1;
        (void)::ioctlsocket(fd, FIONBIO, &mode);
#else
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
#endif
    }

    int create_reuse_port_listener(uint16_t port, bool reuse_port)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }

        int flag = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&flag), sizeof(flag)) != 0) {
            close_socket(fd);
            return -1;
        }
#ifdef SO_REUSEPORT
        if (reuse_port && ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag)) != 0) {
            close_socket(fd);
            return -1;
        }
#else
        if (reuse_port) {
            close_socket(fd);
            return -1;
        }
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (::bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(fd);
            return -1;
        }
        if (::listen(fd, 4096) != 0) {
            close_socket(fd);
            return -1;
        }

        set_nonblocking(fd);
        return fd;
    }

#ifdef YUAN_BENCH_WITH_LIBEV
    struct LibevClient
    {
        ev_io watcher{};
        std::string buffer;
    };

    struct LibevWorker
    {
        struct ev_loop *loop = nullptr;
        ev_io accept_watcher{};
        ev_async stop_watcher{};
        int listen_fd = -1;
        std::thread thread;
    };

    void libev_write_response(int fd)
    {
        (void)::send(fd, kHttpResponseGlobal, static_cast<int>(std::strlen(kHttpResponseGlobal)), 0);
    }

    void libev_client_cb(struct ev_loop *loop, ev_io *watcher, int revents)
    {
        if (!(revents & EV_READ)) {
            return;
        }

        auto *client = static_cast<LibevClient *>(watcher->data);
        if (!client) {
            ev_io_stop(loop, watcher);
            return;
        }

        char chunk[4096];
        const int n = static_cast<int>(::recv(watcher->fd, chunk, sizeof(chunk), 0));
        if (n <= 0) {
            ev_io_stop(loop, watcher);
            close_socket(watcher->fd);
            delete client;
            return;
        }

        client->buffer.append(chunk, static_cast<std::size_t>(n));
        std::string::size_type pos = std::string::npos;
        while ((pos = client->buffer.find("\r\n\r\n")) != std::string::npos) {
            client->buffer.erase(0, pos + 4);
            libev_write_response(watcher->fd);
        }
    }

    void libev_accept_cb(struct ev_loop *loop, ev_io *watcher, int revents)
    {
        if (!(revents & EV_READ)) {
            return;
        }

        for (;;) {
            sockaddr_in addr{};
            socklen_t len = sizeof(addr);
            const int client_fd = ::accept(watcher->fd, reinterpret_cast<sockaddr *>(&addr), &len);
            if (client_fd < 0) {
#ifdef _WIN32
                const int err = yuan::app::GetLastNativeError();
                if (yuan::app::IsNativeRetryableError(err)) {
                    break;
                }
#else
                const int err = yuan::app::GetLastNativeError();
                if (yuan::app::IsNativeRetryableError(err)) {
                    break;
                }
#endif
                break;
            }

            set_client_socket_options(client_fd);
            set_nonblocking(client_fd);

            auto *client = new LibevClient{};
            ev_io_init(&client->watcher, libev_client_cb, client_fd, EV_READ);
            client->watcher.data = client;
            ev_io_start(loop, &client->watcher);
        }
    }

    void libev_stop_cb(struct ev_loop *loop, ev_async *, int)
    {
        ev_break(loop, EVBREAK_ALL);
    }

    struct LibevServer
    {
        std::vector<std::unique_ptr<LibevWorker>> workers;

        bool start(uint16_t port, std::size_t worker_count)
        {
            const std::size_t count = (std::max)(worker_count, std::size_t{1});
            workers.reserve(count);

            for (std::size_t i = 0; i < count; ++i) {
                auto worker = std::make_unique<LibevWorker>();
                worker->loop = ev_loop_new(EVFLAG_AUTO);
                if (!worker->loop) {
                    stop();
                    return false;
                }

                worker->listen_fd = create_reuse_port_listener(port, count > 1);
                if (worker->listen_fd < 0) {
                    stop();
                    return false;
                }

                ev_io_init(&worker->accept_watcher, libev_accept_cb, worker->listen_fd, EV_READ);
                ev_io_start(worker->loop, &worker->accept_watcher);

                ev_async_init(&worker->stop_watcher, libev_stop_cb);
                ev_async_start(worker->loop, &worker->stop_watcher);

                auto *loop = worker->loop;
                worker->thread = std::thread([loop]() {
                    ev_run(loop, 0);
                });

                workers.push_back(std::move(worker));
            }

            return true;
        }

        void stop()
        {
            for (auto &worker : workers) {
                if (worker && worker->loop) {
                    ev_async_send(worker->loop, &worker->stop_watcher);
                }
            }

            for (auto &worker : workers) {
                if (worker && worker->thread.joinable()) {
                    worker->thread.join();
                }
            }

            for (auto &worker : workers) {
                if (!worker) {
                    continue;
                }
                if (worker->loop) {
                    ev_io_stop(worker->loop, &worker->accept_watcher);
                    ev_async_stop(worker->loop, &worker->stop_watcher);
                    ev_loop_destroy(worker->loop);
                    worker->loop = nullptr;
                }
                close_socket(worker->listen_fd);
                worker->listen_fd = -1;
            }
            workers.clear();
        }
    };
#endif

    uint16_t reserve_tcp_port()
    {
        const socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener == kInvalidSocket) {
            return 0;
        }

        int reuse = 1;
#ifdef _WIN32
        (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#else
        (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(listener);
            return 0;
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
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
        const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd == kInvalidSocket) {
            return kInvalidSocket;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(fd);
            return kInvalidSocket;
        }
        set_client_socket_options(fd);
        return fd;
    }

    bool send_all(socket_t fd, const std::string &data)
    {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const auto rc = ::send(fd, data.data() + sent, data.size() - sent, 0);
            if (rc <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(rc);
        }
        return true;
    }

    std::string to_lower_ascii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    std::string::size_type find_case_insensitive(const std::string &haystack, const std::string &needle)
    {
        const auto lower_haystack = to_lower_ascii(haystack);
        const auto lower_needle = to_lower_ascii(needle);
        return lower_haystack.find(lower_needle);
    }

    bool recv_one_response(socket_t fd, std::string &buffer)
    {
        const auto header_end_pos = [&]() -> std::string::size_type {
            while (true) {
                const auto pos = buffer.find("\r\n\r\n");
                if (pos != std::string::npos) {
                    return pos;
                }
                char chunk[4096];
                const auto rc = ::recv(fd, chunk, sizeof(chunk), 0);
                if (rc <= 0) {
                    return std::string::npos;
                }
                buffer.append(chunk, static_cast<std::size_t>(rc));
            }
        }();

        if (header_end_pos == std::string::npos) {
            return false;
        }

        const auto header_block = buffer.substr(0, header_end_pos);
        const auto content_length_header = find_case_insensitive(header_block, "Content-Length:");
        if (content_length_header == std::string::npos || content_length_header > header_end_pos) {
            return false;
        }
        auto value_start = content_length_header + std::strlen("Content-Length:");
        while (value_start < buffer.size() && buffer[value_start] == ' ') {
            ++value_start;
        }
        const auto value_end = buffer.find("\r\n", value_start);
        if (value_end == std::string::npos) {
            return false;
        }
        const auto body_size = static_cast<std::size_t>(std::stoul(buffer.substr(value_start, value_end - value_start)));
        const auto response_size = header_end_pos + 4 + body_size;

        while (buffer.size() < response_size) {
            char chunk[4096];
            const auto rc = ::recv(fd, chunk, sizeof(chunk), 0);
            if (rc <= 0) {
                return false;
            }
            buffer.append(chunk, static_cast<std::size_t>(rc));
        }

        buffer.erase(0, response_size);
        return true;
    }

    std::vector<std::size_t> parse_concurrency_list(const std::string &arg)
    {
        std::vector<std::size_t> values;
        std::stringstream input(arg);
        std::string item;
        while (std::getline(input, item, ',')) {
            if (item.empty()) {
                continue;
            }
            values.push_back(static_cast<std::size_t>(std::stoul(item)));
        }
        values.erase(std::remove(values.begin(), values.end(), 0), values.end());
        if (values.empty()) {
            values = {1, 2, 4, 8, 16, 32, 64, 128};
        }
        return values;
    }

    struct YuanServer
    {
        std::vector<std::unique_ptr<yuan::net::NetworkRuntime>> runtimes;
        std::vector<std::unique_ptr<yuan::server::HttpService>> services;
        std::vector<std::thread> threads;

        void stop()
        {
            for (auto &service : services) {
                service->stop();
            }
            for (auto &runtime : runtimes) {
                runtime->stop();
            }
            for (auto &thread : threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
        }
    };

    struct YuanMinimalServer
    {
        std::vector<std::unique_ptr<yuan::net::NetworkRuntime>> runtimes;
        std::vector<std::unique_ptr<yuan::net::AsyncListenerHost>> listeners;
        std::vector<std::thread> threads;

        bool start(uint16_t port, std::size_t worker_count)
        {
            auto log_registry = yuan::log::LogRegistry::get_instance();
            log_registry->disable_file_log();
            log_registry->set_global_level(yuan::log::Level::fatal);
            log_registry->set_default(nullptr);

            const std::size_t count = (std::max)(worker_count, std::size_t{1});
            runtimes.reserve(count);
            listeners.reserve(count);
            threads.reserve(count);

            for (std::size_t i = 0; i < count; ++i) {
                auto runtime = std::make_unique<yuan::net::NetworkRuntime>();
                auto listener = std::make_unique<yuan::net::AsyncListenerHost>();

                listener->set_connection_handler([](yuan::net::AsyncConnectionContext ctx) -> yuan::coroutine::Task<void> {
                    std::string input;
                    while (ctx.is_connected()) {
                        auto read = co_await ctx.read_async();
                        if (read.status != yuan::coroutine::IoStatus::success) {
                            break;
                        }

                        const auto span = read.data.readable_span();
                        input.append(span.data(), span.size());
                        for (;;) {
                            const auto request_end = input.find("\r\n\r\n");
                            if (request_end == std::string::npos) {
                                break;
                            }
                            input.erase(0, request_end + 4);
                            ctx.append_output(kHttpResponse);
                            ctx.flush();
                        }
                    }
                    ctx.close();
                    co_return;
                });

                yuan::net::ListenOptions options;
                options.reuse_port = count > 1;
                options.backlog = 4096;
                if (!listener->bind("127.0.0.1", port, *runtime, options)) {
                    stop();
                    return false;
                }

                auto task = listener->run_async();
                task.resume();
                task.detach();

                listeners.push_back(std::move(listener));
                runtimes.push_back(std::move(runtime));
            }

            for (auto &runtime : runtimes) {
                threads.emplace_back([runtime = runtime.get()]() {
                    runtime->run();
                });
            }

            return true;
        }

        void stop()
        {
            for (auto &listener : listeners) {
                if (listener) {
                    listener->close();
                }
            }
            for (auto &runtime : runtimes) {
                if (runtime) {
                    runtime->stop();
                }
            }
            for (auto &thread : threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            threads.clear();
            listeners.clear();
            runtimes.clear();
        }
    };

    class YuanRawConnectionHandler final : public yuan::net::ConnectionHandler
    {
    public:
        void on_connected(const std::shared_ptr<yuan::net::Connection> &) override
        {
        }

        void on_error(const std::shared_ptr<yuan::net::Connection> &conn) override
        {
            if (conn) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    buffers_.erase(conn.get());
                }
                conn->close();
            }
        }

        void on_read(const std::shared_ptr<yuan::net::Connection> &conn) override
        {
            if (!conn) {
                return;
            }

            auto input = conn->take_input_byte_buffer();
            const auto span = input.readable_span();
            if (span.empty()) {
                return;
            }

            std::size_t completed = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto &buffer = buffers_[conn.get()];
                buffer.append(span.data(), span.size());
                std::string::size_type pos = std::string::npos;
                while ((pos = buffer.find("\r\n\r\n")) != std::string::npos) {
                    buffer.erase(0, pos + 4);
                    ++completed;
                }
            }

            for (std::size_t i = 0; i < completed; ++i) {
                conn->append_output(kHttpResponse);
            }
            if (completed > 0) {
                conn->flush();
            }
        }

        void on_write(const std::shared_ptr<yuan::net::Connection> &) override
        {
        }

        void on_close(const std::shared_ptr<yuan::net::Connection> &conn) override
        {
            if (!conn) {
                return;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            buffers_.erase(conn.get());
        }

    private:
        std::mutex mutex_;
        std::unordered_map<yuan::net::Connection *, std::string> buffers_;
    };

    struct YuanRawServer
    {
        std::vector<std::unique_ptr<yuan::net::NetworkRuntime>> runtimes;
        std::vector<std::unique_ptr<yuan::net::StreamAcceptor>> acceptors;
        std::vector<std::unique_ptr<YuanRawConnectionHandler>> handlers;
        std::vector<std::thread> threads;

        bool start(uint16_t port, std::size_t worker_count)
        {
            auto log_registry = yuan::log::LogRegistry::get_instance();
            log_registry->disable_file_log();
            log_registry->set_global_level(yuan::log::Level::fatal);
            log_registry->set_default(nullptr);

            const std::size_t count = (std::max)(worker_count, std::size_t{1});
            runtimes.reserve(count);
            acceptors.reserve(count);
            handlers.reserve(count);
            threads.reserve(count);

            for (std::size_t i = 0; i < count; ++i) {
                auto runtime = std::make_unique<yuan::net::NetworkRuntime>();
                auto socket = std::make_unique<yuan::net::Socket>("127.0.0.1", port);
                if (!socket->valid()) {
                    stop();
                    return false;
                }

                yuan::net::ListenOptions options;
                options.reuse_port = count > 1;
                options.backlog = 4096;
                if (!socket->apply_listen_options(options) || !socket->bind()) {
                    stop();
                    return false;
                }

                auto acceptor = std::unique_ptr<yuan::net::StreamAcceptor>(yuan::net::create_stream_acceptor(socket.release()));
                if (!acceptor || !acceptor->listen(options.backlog)) {
                    stop();
                    return false;
                }

                auto handler = std::make_unique<YuanRawConnectionHandler>();
                acceptor->set_connection_handler(yuan::net::make_non_owning_handler(handler.get()));
                acceptor->set_event_handler(runtime->event_loop());
                if (auto *channel = acceptor->listener_channel()) {
                    runtime->update_channel(channel);
                }

                handlers.push_back(std::move(handler));
                acceptors.push_back(std::move(acceptor));
                runtimes.push_back(std::move(runtime));
            }

            for (auto &runtime : runtimes) {
                threads.emplace_back([runtime = runtime.get()]() {
                    runtime->run();
                });
            }
            return true;
        }

        void stop()
        {
            for (auto &acceptor : acceptors) {
                if (acceptor) {
                    acceptor->close();
                }
            }
            for (auto &runtime : runtimes) {
                if (runtime) {
                    runtime->stop();
                }
            }
            for (auto &thread : threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            threads.clear();
            acceptors.clear();
            runtimes.clear();
            handlers.clear();
        }
    };

    bool start_yuan_server(YuanServer &server,
                           uint16_t port,
                           std::size_t worker_count,
                           bool use_iocp_backend = false)
    {
        auto log_registry = yuan::log::LogRegistry::get_instance();
        log_registry->disable_file_log();
        log_registry->set_global_level(yuan::log::Level::fatal);
        log_registry->set_default(nullptr);

        auto bus = std::make_shared<yuan::eventbus::EventBus>();
        const std::size_t service_count = use_iocp_backend ? 1 : worker_count;
        server.runtimes.reserve(service_count);
        server.services.reserve(service_count);
        server.threads.reserve(service_count);

        for (std::size_t i = 0; i < service_count; ++i) {
            auto runtime = std::make_unique<yuan::net::NetworkRuntime>();

            yuan::net::http::HttpServerConfig cfg;
            cfg.enable_ssl = false;
            cfg.enable_cors = false;
            cfg.enable_keep_alive = true;
            if (use_iocp_backend) {
                cfg.listen_options.use_iocp = true;
                cfg.listen_options.iocp_worker_count = worker_count;
            }
            auto service = std::make_unique<yuan::server::HttpService>(port, cfg);

            yuan::app::RuntimeContext context;
            context.app_name = "http-worker-pool-benchmark";
            context.run_mode = yuan::app::RunMode::multi_process;
            context.worker_threads = worker_count;
            context.runtime_worker_count = worker_count;
            context.worker_index = i;
            context.is_worker_process = true;
            context.active_service_name = "http";
            context.service_index = 0;
            context.service_instance_index = i;
            context.service_instance_count = service_count;
            context.listener_reuse_port = !use_iocp_backend && worker_count > 1;
            context.shared_runtime = runtime.get();
            context.event_bus = bus;
            service->set_runtime_context(context);

            service->server().on("/bench", [](yuan::net::http::HttpRequest *,
                                               yuan::net::http::HttpResponse *resp) {
                resp->send_body(kBenchmarkBody, "text/plain");
            });

            if (!service->init()) {
                return false;
            }

            server.runtimes.push_back(std::move(runtime));
            server.services.push_back(std::move(service));
        }

        for (auto &service : server.services) {
            service->start();
        }
        for (auto &runtime : server.runtimes) {
            server.threads.emplace_back([runtime = runtime.get()]() {
                runtime->run();
            });
        }
        return true;
    }

#ifdef YUAN_BENCH_WITH_LIBEVENT
    void libevent_http_cb(evhttp_request *request, void *)
    {
        auto *body = evbuffer_new();
        if (!body) {
            evhttp_send_error(request, HTTP_INTERNAL, "allocation failed");
            return;
        }

        auto *headers = evhttp_request_get_output_headers(request);
        evhttp_add_header(headers, "Content-Type", "text/plain");
        evhttp_add_header(headers, "Content-Length", "2");
        evbuffer_add(body, kBenchmarkBody, kBenchmarkBodySize);
        evhttp_send_reply(request, HTTP_OK, "OK", body);
        evbuffer_free(body);
    }

    struct LibeventWorker
    {
        event_base *base = nullptr;
        evhttp *http = nullptr;
        evhttp_bound_socket *bound = nullptr;
        std::thread thread;
    };

    struct LibeventServer
    {
        std::vector<std::unique_ptr<LibeventWorker>> workers;

        bool start(uint16_t port, std::size_t worker_count)
        {
            static std::once_flag libevent_thread_init;
            std::call_once(libevent_thread_init, []() {
#ifdef _WIN32
                evthread_use_windows_threads();
#else
                evthread_use_pthreads();
#endif
            });

            workers.reserve(worker_count);
            for (std::size_t i = 0; i < worker_count; ++i) {
                auto worker = std::make_unique<LibeventWorker>();
                worker->base = event_base_new();
                if (!worker->base) {
                    stop();
                    return false;
                }

                worker->http = evhttp_new(worker->base);
                if (!worker->http) {
                    stop();
                    return false;
                }
                evhttp_set_gencb(worker->http, libevent_http_cb, nullptr);

                const int fd = create_reuse_port_listener(port, worker_count > 1);
                if (fd < 0) {
                    stop();
                    return false;
                }
                worker->bound = evhttp_accept_socket_with_handle(worker->http, fd);
                if (!worker->bound) {
                    close_socket(fd);
                    stop();
                    return false;
                }

                auto *base = worker->base;
                worker->thread = std::thread([base]() {
                    event_base_dispatch(base);
                });
                workers.push_back(std::move(worker));
            }
            return true;
        }

        void stop()
        {
            for (auto &worker : workers) {
                if (worker && worker->base) {
                    event_base_loopbreak(worker->base);
                }
            }
            for (auto &worker : workers) {
                if (worker && worker->thread.joinable()) {
                    worker->thread.join();
                }
            }
            for (auto &worker : workers) {
                if (!worker) {
                    continue;
                }
                if (worker->http) {
                    evhttp_free(worker->http);
                    worker->http = nullptr;
                }
                if (worker->base) {
                    event_base_free(worker->base);
                    worker->base = nullptr;
                }
            }
            workers.clear();
        }
    };
#endif

#ifdef YUAN_BENCH_WITH_LIBUV
    struct UvClient
    {
        uv_tcp_t handle{};
        std::string buffer;
    };

    struct UvWorker
    {
        uv_loop_t *loop = nullptr;
        uv_tcp_t server{};
        uv_async_t stop_async{};
        std::thread thread;
    };

    void uv_alloc_cb(uv_handle_t *, std::size_t suggested_size, uv_buf_t *buf)
    {
        buf->base = static_cast<char *>(std::malloc(suggested_size));
        buf->len = suggested_size;
    }

    void uv_write_done(uv_write_t *request, int)
    {
        delete request;
    }

    void uv_client_closed(uv_handle_t *handle)
    {
        delete static_cast<UvClient *>(handle->data);
    }

    void uv_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
    {
        auto *client = static_cast<UvClient *>(stream->data);
        if (nread > 0 && client) {
            client->buffer.append(buf->base, static_cast<std::size_t>(nread));
            std::string::size_type pos = std::string::npos;
            while ((pos = client->buffer.find("\r\n\r\n")) != std::string::npos) {
                client->buffer.erase(0, pos + 4);
                auto *request = new uv_write_t{};
                uv_buf_t response = uv_buf_init(const_cast<char *>(kHttpResponse),
                                                static_cast<unsigned int>(std::strlen(kHttpResponse)));
                if (uv_write(request, stream, &response, 1, uv_write_done) != 0) {
                    delete request;
                    break;
                }
            }
        } else if (nread < 0 && !uv_is_closing(reinterpret_cast<uv_handle_t *>(stream))) {
            uv_close(reinterpret_cast<uv_handle_t *>(stream), uv_client_closed);
        }

        std::free(buf->base);
    }

    void uv_connection_cb(uv_stream_t *server, int status)
    {
        if (status < 0) {
            return;
        }

        auto *client = new UvClient{};
        uv_tcp_init(server->loop, &client->handle);
        client->handle.data = client;

        if (uv_accept(server, reinterpret_cast<uv_stream_t *>(&client->handle)) == 0) {
            uv_tcp_nodelay(&client->handle, 1);
            uv_read_start(reinterpret_cast<uv_stream_t *>(&client->handle), uv_alloc_cb, uv_read_cb);
        } else {
            uv_close(reinterpret_cast<uv_handle_t *>(&client->handle), uv_client_closed);
        }
    }

    void uv_close_walk_cb(uv_handle_t *handle, void *)
    {
        if (!uv_is_closing(handle)) {
            if (handle->data && handle->type == UV_TCP && handle->data != handle->loop->data) {
                uv_close(handle, uv_client_closed);
            } else {
                uv_close(handle, nullptr);
            }
        }
    }

    void uv_stop_async_cb(uv_async_t *async)
    {
        uv_stop(async->loop);
    }

    struct LibuvServer
    {
        std::vector<std::unique_ptr<UvWorker>> workers;

        bool start(uint16_t port, std::size_t worker_count)
        {
            workers.reserve(worker_count);
            std::vector<std::shared_ptr<std::promise<bool>>> ready;
            ready.reserve(worker_count);

            for (std::size_t i = 0; i < worker_count; ++i) {
                auto worker = std::make_unique<UvWorker>();
                auto signal = std::make_shared<std::promise<bool>>();
                ready.push_back(signal);
                auto *state = worker.get();

                state->thread = std::thread([state, signal, port, worker_count]() {
                    state->loop = new uv_loop_t{};
                    uv_loop_init(state->loop);
                    state->loop->data = state;

                    bool ok = true;
                    sockaddr_in addr{};
                    uv_ip4_addr("127.0.0.1", port, &addr);
                    ok = ok && uv_tcp_init(state->loop, &state->server) == 0;
                    state->server.data = state;
                    const unsigned int flags = worker_count > 1 ? UV_TCP_REUSEPORT : 0;
                    ok = ok && uv_tcp_bind(&state->server, reinterpret_cast<const sockaddr *>(&addr), flags) == 0;
                    ok = ok && uv_listen(reinterpret_cast<uv_stream_t *>(&state->server), 4096, uv_connection_cb) == 0;
                    ok = ok && uv_async_init(state->loop, &state->stop_async, uv_stop_async_cb) == 0;
                    state->stop_async.data = state;

                    signal->set_value(ok);
                    if (ok) {
                        uv_run(state->loop, UV_RUN_DEFAULT);
                    }

                    uv_walk(state->loop, uv_close_walk_cb, nullptr);
                    uv_run(state->loop, UV_RUN_DEFAULT);
                    uv_loop_close(state->loop);
                    delete state->loop;
                    state->loop = nullptr;
                });

                workers.push_back(std::move(worker));
            }

            for (auto &signal : ready) {
                if (!signal->get_future().get()) {
                    stop();
                    return false;
                }
            }
            return true;
        }

        void stop()
        {
            for (auto &worker : workers) {
                if (worker && worker->loop) {
                    uv_async_send(&worker->stop_async);
                }
            }
            for (auto &worker : workers) {
                if (worker && worker->thread.joinable()) {
                    worker->thread.join();
                }
            }
            workers.clear();
        }
    };
#endif

#ifdef _WIN32
    struct IocpServer
    {
        struct ClientState
        {
            std::string input;
        };

        yuan::net::IocpTcpEngine engine;
        std::shared_ptr<const std::string> response;

        bool start(uint16_t port, std::size_t worker_count)
        {
            response = std::make_shared<const std::string>(kHttpResponse);
            yuan::net::IocpTcpEngineCallbacks callbacks;
            callbacks.on_accept = [](const std::shared_ptr<yuan::net::IocpTcpConnection> &connection) {
                connection->set_user_data(std::make_shared<ClientState>());
            };
            callbacks.on_read = [response = response](const std::shared_ptr<yuan::net::IocpTcpConnection> &connection,
                                                      const char *data,
                                                      std::size_t size) {
                auto state = std::static_pointer_cast<ClientState>(connection->user_data());
                if (!state) {
                    state = std::make_shared<ClientState>();
                    connection->set_user_data(state);
                }

                state->input.append(data, size);
                for (;;) {
                    const auto request_end = state->input.find("\r\n\r\n");
                    if (request_end == std::string::npos) {
                        break;
                    }
                    state->input.erase(0, request_end + 4);
                    connection->send_shared(response);
                }
            };
            return engine.listen("127.0.0.1", port, worker_count, std::move(callbacks));
        }

        void stop()
        {
            engine.stop();
        }
    };

    struct YuanIocpMinimalServer
    {
        yuan::net::NetworkRuntime runtime;
        yuan::net::AsyncListenerHost listener;
        std::thread thread;

        bool start(uint16_t port, std::size_t worker_count)
        {
            yuan::net::ListenOptions options;
            options.use_iocp = true;
            options.iocp_worker_count = worker_count;

            listener.set_connection_handler([](yuan::net::AsyncConnectionContext ctx)->yuan::coroutine::Task<void> {
                std::string input;
                while (ctx.is_connected()) {
                    auto read = co_await ctx.read_async();
                    if (read.status != yuan::coroutine::IoStatus::success) {
                        break;
                    }
                    const auto span = read.data.readable_span();
                    input.append(span.data(), span.size());
                    for (;;) {
                        const auto request_end = input.find("\r\n\r\n");
                        if (request_end == std::string::npos) {
                            break;
                        }
                        input.erase(0, request_end + 4);
                        ctx.append_output(kHttpResponse);
                        ctx.flush();
                    }
                }
                ctx.close();
                co_return;
            });

            if (!listener.bind("127.0.0.1", port, runtime, options)) {
                return false;
            }

            auto task = listener.run_async();
            task.resume();
            task.detach();
            thread = std::thread([this]() {
                runtime.run();
            });
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

    struct YuanIocpShardedMinimalServer
    {
        yuan::net::IocpShardedAsyncListener listener;

        static yuan::coroutine::Task<void> handle_connection(yuan::net::AsyncConnectionContext ctx)
        {
            std::string input;
            while (ctx.is_connected()) {
                auto read = co_await ctx.read_async();
                if (read.status != yuan::coroutine::IoStatus::success) {
                    break;
                }
                const auto span = read.data.readable_span();
                input.append(span.data(), span.size());
                for (;;) {
                    const auto request_end = input.find("\r\n\r\n");
                    if (request_end == std::string::npos) {
                        break;
                    }
                    input.erase(0, request_end + 4);
                    ctx.append_output(kHttpResponse);
                    ctx.flush();
                }
            }
            ctx.close();
            co_return;
        }

        bool start(uint16_t port, std::size_t worker_count)
        {
            return listener.listen("127.0.0.1", port, worker_count, worker_count, 1, handle_connection);
        }

        void stop()
        {
            listener.close();
        }
    };
#endif

#ifdef YUAN_BENCH_WITH_WORKFLOW
    struct WorkflowServer
    {
        std::unique_ptr<WFHttpServer> server;
        std::string body = kBenchmarkBody;

        bool start(uint16_t port, std::size_t worker_count)
        {
            WFGlobalSettings settings = GLOBAL_SETTINGS_DEFAULT;
            settings.poller_threads = static_cast<int>(worker_count);
            WORKFLOW_library_init(&settings);

            server = std::make_unique<WFHttpServer>([this](WFHttpTask *task) {
                auto *response = task->get_resp();
                response->add_header_pair("Content-Type", "text/plain");
                response->add_header_pair("Content-Length", "2");
                response->append_output_body_nocopy(body.data(), body.size());
            });

            return server->start(port) == 0;
        }

        void stop()
        {
            if (server) {
                server->stop();
                server.reset();
            }
        }
    };
#endif

    struct BenchmarkServer
    {
        std::string implementation;
        YuanServer yuan;
        YuanMinimalServer yuan_min;
        YuanRawServer yuan_raw;
#ifdef YUAN_BENCH_WITH_LIBEVENT
        LibeventServer libevent;
#endif
#ifdef YUAN_BENCH_WITH_LIBUV
        LibuvServer libuv;
#endif
#ifdef YUAN_BENCH_WITH_LIBEV
        LibevServer libev;
#endif
#ifdef YUAN_BENCH_WITH_LIBHV
        LibhvServer libhv;
#endif
#ifdef _WIN32
        IocpServer iocp;
        YuanIocpMinimalServer yuan_iocp_min;
        YuanIocpShardedMinimalServer yuan_iocp_sharded_min;
#endif
#ifdef YUAN_BENCH_WITH_WORKFLOW
        WorkflowServer workflow;
#endif

        bool start(const std::string &name, uint16_t port, std::size_t worker_count)
        {
            implementation = name;
            if (name == "yuan") {
                return start_yuan_server(yuan, port, worker_count);
            }
            if (name == "yuan_min") {
                return yuan_min.start(port, worker_count);
            }
            if (name == "yuan_raw") {
                return yuan_raw.start(port, worker_count);
            }
            if (name == "yuan_iocp") {
                return start_yuan_server(yuan, port, worker_count, true);
            }
#ifdef YUAN_BENCH_WITH_LIBEVENT
            if (name == "libevent") {
                return libevent.start(port, worker_count);
            }
#endif
#ifdef YUAN_BENCH_WITH_LIBUV
            if (name == "libuv") {
                return libuv.start(port, worker_count);
            }
#endif
#ifdef YUAN_BENCH_WITH_LIBEV
            if (name == "libev") {
                return libev.start(port, worker_count);
            }
#endif
#ifdef YUAN_BENCH_WITH_LIBHV
            if (name == "libhv") {
                return libhv.start(port, worker_count);
            }
#endif
#ifdef _WIN32
            if (name == "yuan_iocp_min") {
                return yuan_iocp_min.start(port, worker_count);
            }
            if (name == "yuan_iocp_sharded_min") {
                return yuan_iocp_sharded_min.start(port, worker_count);
            }
            if (name == "iocp") {
                return iocp.start(port, worker_count);
            }
#endif
#ifdef YUAN_BENCH_WITH_WORKFLOW
            if (name == "workflow") {
                return workflow.start(port, worker_count);
            }
#endif
            return false;
        }

        void stop()
        {
            if (implementation == "yuan" || implementation == "yuan_iocp") {
                yuan.stop();
            }
            else if (implementation == "yuan_min") {
                yuan_min.stop();
            }
            else if (implementation == "yuan_raw") {
                yuan_raw.stop();
            }
#ifdef YUAN_BENCH_WITH_LIBEVENT
            else if (implementation == "libevent") {
                libevent.stop();
            }
#endif
#ifdef YUAN_BENCH_WITH_LIBUV
            else if (implementation == "libuv") {
                libuv.stop();
            }
#endif
#ifdef YUAN_BENCH_WITH_LIBEV
            else if (implementation == "libev") {
                libev.stop();
            }
#endif
#ifdef YUAN_BENCH_WITH_LIBHV
            else if (implementation == "libhv") {
                libhv.stop();
            }
#endif
#ifdef _WIN32
            else if (implementation == "yuan_iocp_min") {
                yuan_iocp_min.stop();
            }
            else if (implementation == "yuan_iocp_sharded_min") {
                yuan_iocp_sharded_min.stop();
            }
            else if (implementation == "iocp") {
                iocp.stop();
            }
#endif
#ifdef YUAN_BENCH_WITH_WORKFLOW
            else if (implementation == "workflow") {
                workflow.stop();
            }
#endif
        }
    };

    std::uint64_t run_load(uint16_t port, std::size_t concurrency, std::chrono::seconds duration)
    {
        std::atomic_bool stop{false};
        std::atomic<std::uint64_t> requests{0};
        std::vector<std::thread> clients;
        clients.reserve(concurrency);
        const std::string request =
            "GET /bench HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: keep-alive\r\n\r\n";

        for (std::size_t i = 0; i < concurrency; ++i) {
            clients.emplace_back([&]() {
                std::string read_buffer;
                socket_t fd = kInvalidSocket;
                while (!stop.load(std::memory_order_relaxed)) {
                    if (fd == kInvalidSocket) {
                        fd = connect_loopback(port);
                        if (fd == kInvalidSocket) {
                            continue;
                        }
                        read_buffer.clear();
                    }
                    if (!send_all(fd, request) || !recv_one_response(fd, read_buffer)) {
                        close_socket(fd);
                        fd = kInvalidSocket;
                        continue;
                    }
                    requests.fetch_add(1, std::memory_order_relaxed);
                }
                close_socket(fd);
            });
        }

        std::this_thread::sleep_for(duration);
        stop.store(true, std::memory_order_release);
        for (auto &client : clients) {
            if (client.joinable()) {
                client.join();
            }
        }
        return requests.load(std::memory_order_acquire);
    }
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 1;
    }
#endif

    const auto hardware = std::max<>(1u, std::thread::hardware_concurrency());
    const auto implementation = argc > 1 ? std::string(argv[1]) : std::string("yuan");
    const auto worker_count = argc > 2 ? static_cast<std::size_t>(std::stoul(argv[2]))
                                       : static_cast<std::size_t>((std::min)(hardware, 4u));
    const auto duration = argc > 3 ? std::chrono::seconds(std::stoul(argv[3])) : std::chrono::seconds(3);
    const auto concurrencies = argc > 4
        ? parse_concurrency_list(argv[4])
        : std::vector<std::size_t>{1, 2, 4, 8, 16, 32, 64, 128};

    BenchmarkServer server;
    uint16_t port = 0;
    constexpr int kStartAttempts = 5;
    for (int attempt = 1; attempt <= kStartAttempts; ++attempt) {
        port = reserve_tcp_port();
        if (port == 0) {
            std::cerr << "failed to reserve benchmark port attempt=" << attempt << '\n';
            continue;
        }
        if (server.start(implementation, port, worker_count)) {
            break;
        }
        std::cerr << "failed to start HTTP benchmark server implementation=" << implementation
                  << " port=" << port
                  << " attempt=" << attempt << '\n';
        server.stop();
        port = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (port == 0) {
        std::cerr << "failed to start HTTP benchmark server implementation=" << implementation
                  << " attempts=" << kStartAttempts << '\n';
        return 1;
    }

    std::cout << "http worker-pool benchmark implementation=" << implementation << " workers=" << worker_count
              << " duration_s=" << duration.count() << '\n';
    std::cout << "concurrency,requests,requests_per_second\n";
    double best_rps = 0.0;
    std::size_t best_concurrency = 0;
    for (const auto concurrency : concurrencies) {
        const auto started = Clock::now();
        const auto requests = run_load(port, concurrency, duration);
        const auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(Clock::now() - started).count();
        const auto rps = elapsed > 0.0 ? static_cast<double>(requests) / elapsed : 0.0;
        if (rps > best_rps) {
            best_rps = rps;
            best_concurrency = concurrency;
        }
        std::cout << concurrency << ',' << requests << ',' << rps << '\n';
    }
    std::cout << "best_concurrency=" << best_concurrency << " best_requests_per_second=" << best_rps << '\n';

    server.stop();
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
