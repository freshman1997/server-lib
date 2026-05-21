#include "application.h"
#include "bootstrap.h"
#include "ftp/ftp_service.h"

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
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    yuan::app::RuntimeContext context;
    context.app_name = "ftp-test-server";

    yuan::app::Application application(context);
    auto service = std::make_shared<yuan::server::FtpService>(12123);
    if (!application.add_typed_service<yuan::server::FtpService>("ftp", service, "server.ftp", 1)) {
        std::cerr << "failed to register ftp service\n";
        return 1;
    }

    yuan::app::Bootstrap bootstrap(application);
    if (!bootstrap.run()) {
        std::cerr << "failed to start ftp service\n";
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
