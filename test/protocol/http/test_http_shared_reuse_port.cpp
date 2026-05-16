#include "http_service.h"

#include <iostream>

#ifdef _WIN32
int main()
{
    std::cout << "http shared reuse_port test skipped on Windows\n";
    return 0;
}
#else
#include "eventbus/event_bus.h"
#include "net/runtime/network_runtime.h"
#include "runtime_context.h"
#include "server_service_events.h"

#include <any>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
    int g_failed = 0;

    void check(bool cond, const char *message)
    {
        if (!cond) {
            ++g_failed;
            std::cerr << "[FAIL] " << message << '\n';
        }
    }

    void close_socket(int fd)
    {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    bool send_all(int fd, const std::string &data)
    {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t rc = ::send(fd, data.data() + sent, data.size() - sent, 0);
            if (rc <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(rc);
        }
        return true;
    }

    std::string recv_all(int fd)
    {
        std::string out;
        char buf[2048];
        while (true) {
            const ssize_t rc = ::recv(fd, buf, sizeof(buf), 0);
            if (rc <= 0) {
                break;
            }
            out.append(buf, static_cast<std::size_t>(rc));
            if (out.find("\r\n\r\n") != std::string::npos &&
                out.find("worker=") != std::string::npos) {
                break;
            }
        }
        return out;
    }

    uint16_t reserve_tcp_port()
    {
        const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) {
            return 0;
        }

        int reuse = 1;
        (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

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

        const uint16_t port = ntohs(bound.sin_port);
        close_socket(listener);
        return port;
    }

    std::string http_get(uint16_t port)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return {};
        }

        timeval tv{};
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(fd);
            return {};
        }

        const std::string req =
            "GET /reuse-port-worker HTTP/1.1\r\n"
            "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
            "Connection: close\r\n\r\n";
        if (!send_all(fd, req)) {
            close_socket(fd);
            return {};
        }

        auto response = recv_all(fd);
        close_socket(fd);
        return response;
    }
}

int main()
{
    constexpr std::size_t kWorkerCount = 4;
    const uint16_t port = reserve_tcp_port();
    check(port != 0, "shared reuse_port test should reserve a TCP port");
    if (port == 0) {
        return 1;
    }

    auto bus = std::make_shared<yuan::eventbus::EventBus>();
    std::set<std::size_t> activated_instances;
    bus->subscribe(yuan::server::events::service_activated,
        [&](const yuan::eventbus::Event &event) {
            if (const auto *payload = std::any_cast<yuan::server::ServiceRuntimeEvent>(&event.payload)) {
                if (payload->service_name == "http") {
                    activated_instances.insert(payload->service_instance_index);
                }
            }
        });

    std::vector<std::unique_ptr<yuan::net::NetworkRuntime>> runtimes;
    std::vector<std::unique_ptr<yuan::server::HttpService>> services;
    runtimes.reserve(kWorkerCount);
    services.reserve(kWorkerCount);

    for (std::size_t i = 0; i < kWorkerCount; ++i) {
        auto runtime = std::make_unique<yuan::net::NetworkRuntime>();

        yuan::net::http::HttpServerConfig cfg;
        cfg.enable_keep_alive = false;
        auto service = std::make_unique<yuan::server::HttpService>(port, cfg);

        yuan::app::RuntimeContext context;
        context.app_name = "http-reuse-port-test";
        context.run_mode = yuan::app::RunMode::multi_process;
        context.worker_threads = kWorkerCount;
        context.runtime_worker_count = kWorkerCount;
        context.worker_index = i;
        context.is_worker_process = true;
        context.active_service_name = "http";
        context.service_index = 0;
        context.service_instance_index = i;
        context.service_instance_count = kWorkerCount;
        context.listener_reuse_port = true;
        context.shared_runtime = runtime.get();
        context.event_bus = bus;
        service->set_runtime_context(context);

        const auto worker_label = std::to_string(i);
        service->server().on("/reuse-port-worker",
            [worker_label](yuan::net::http::HttpRequest *,
                           yuan::net::http::HttpResponse *resp) {
                const std::string body = "worker=" + worker_label;
                resp->set_response_code(yuan::net::http::ResponseCode::ok_);
                resp->add_header("Content-Type", "text/plain");
                resp->add_header("Content-Length", std::to_string(body.size()));
                resp->append_body(body);
                resp->send();
            });

        check(service->init(), "shared reuse_port HTTP service should init");
        runtimes.push_back(std::move(runtime));
        services.push_back(std::move(service));
    }

    if (g_failed != 0) {
        return 1;
    }

    for (auto &service : services) {
        service->start();
    }
    check(activated_instances.size() == kWorkerCount,
          "shared reuse_port startup should activate every HTTP service instance");

    std::vector<std::thread> runtime_threads;
    runtime_threads.reserve(kWorkerCount);
    for (auto &runtime : runtimes) {
        runtime_threads.emplace_back([runtime = runtime.get()]() {
            runtime->run();
        });
    }

    std::set<std::string> seen_workers;
    for (int i = 0; i < 64; ++i) {
        const auto response = http_get(port);
        if (response.find("200") != std::string::npos &&
            response.find("worker=") != std::string::npos) {
            const auto pos = response.find("worker=");
            seen_workers.insert(response.substr(pos, 8));
        }
    }

    check(!seen_workers.empty(), "shared reuse_port HTTP endpoint should handle requests");

    for (auto &service : services) {
        service->stop();
    }
    for (auto &runtime : runtimes) {
        runtime->stop();
    }
    for (auto &thread : runtime_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    if (g_failed != 0) {
        std::cerr << "http shared reuse_port test failed=" << g_failed << '\n';
        return 1;
    }

    std::cout << "http shared reuse_port test passed, seen_workers=" << seen_workers.size() << '\n';
    return 0;
}
#endif
