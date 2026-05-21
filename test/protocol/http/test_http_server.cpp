#include "application.h"
#include "bootstrap.h"
#include "http/http_service.h"


#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

namespace
{

std::atomic<bool> g_running{true};

void signal_handler(int)
{
    g_running.store(false);
}

} // namespace

int main()
{
    

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    yuan::app::RuntimeContext context;
    context.worker_threads = 2;
    context.app_name = "http-test-server";

    yuan::app::Application application(context);
    yuan::net::http::HttpServerConfig http_config;
    http_config.enable_ssl = false;
    auto service = std::make_shared<yuan::server::HttpService>(45005, http_config);
    if (!application.add_typed_service<yuan::server::HttpService>("http", service, "server.http", 1)) {
        std::cerr << "failed to register http service\n";
        return 1;
    }

    yuan::net::http::HttpServerConfig https_config;
    https_config.enable_ssl = true;
    auto https_service = std::make_shared<yuan::server::HttpService>(45006, https_config);
    if (!application.add_typed_service<yuan::server::HttpService>("https", https_service, "server.https", 2)) {
        std::cerr << "failed to register https service\n";
        return 1;
    }

    yuan::app::Bootstrap bootstrap(application);
    if (!bootstrap.run()) {
        std::cerr << "failed to start http service\n";
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
