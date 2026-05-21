#include "application.h"
#include "bootstrap.h"
#include "buffer/byte_buffer.h"
#include "websocket/websocket_service.h"
#include "websocket.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

using namespace yuan;

namespace
{

std::atomic<bool> g_running{true};

void signal_handler(int)
{
    g_running.store(false);
}

class TestServer : public net::websocket::WebSocketDataHandler
{
public:
    void on_connected(net::websocket::WebSocketConnection *wsConn) override
    {
        (void)wsConn;
    }

    void on_data(net::websocket::WebSocketConnection *wsConn, const ::yuan::buffer::ByteBuffer &buff) override
    {
        std::ofstream file("data.txt");
        if (file.good()) {
            file.write(buff.read_ptr(), buff.readable_bytes());
        }

        wsConn->send(buff);
    }

    void on_close(net::websocket::WebSocketConnection *wsConn) override
    {
        (void)wsConn;
    }
};

} // namespace

int main()
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    app::RuntimeContext context;
    context.app_name = "websocket-test-server";

    app::Application application(context);
    auto service = std::make_shared<server::WebSocketService>(12211);

    TestServer handler;
    service->set_data_handler(&handler);

    if (!application.add_typed_service<server::WebSocketService>(
            "websocket",
            service,
            "server.websocket",
            1)) {
        std::cerr << "failed to register websocket service\n";
        return 1;
    }

    app::Bootstrap bootstrap(application);
    if (!bootstrap.run()) {
        std::cerr << "failed to start websocket service\n";
        return 1;
    }

    const auto snapshot = bootstrap.supervisor_snapshot();
    std::cout << "process role: " << yuan::app::to_string(bootstrap.process_role())
              << ", supervisor_state=" << yuan::app::to_string(snapshot.state)
              << ", supervisor_reason=" << yuan::app::to_string(snapshot.reason)
              << ", worker_index=" << application.context().worker_index
              << ", is_worker_process=" << (application.context().is_worker_process ? "true" : "false")
              << ", owns_runtime=" << bootstrap.owns_runtime()
              << ", running_workers=" << snapshot.running_workers
              << ", recovering_workers=" << snapshot.recovering_workers
              << ", suppressed_workers=" << snapshot.suppressed_workers
              << ", failed_workers=" << snapshot.failed_workers
              << ", total_restarts=" << snapshot.total_restarts
              << ", shutdown_started=" << (snapshot.shutdown_started ? "true" : "false")
              << '\n';

    while (g_running.load()) {
        bootstrap.poll_workers();
        if (bootstrap.process_role() == yuan::app::ProcessRole::supervisor &&
            (bootstrap.has_failed_workers() ||
             (!bootstrap.has_running_workers() && !bootstrap.has_recovering_workers()))) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    bootstrap.shutdown();
    return 0;
}
