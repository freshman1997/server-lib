#include "application.h"
#include "bootstrap.h"
#include "dns_service.h"
#include "logger.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

#ifdef _WIN32
#include <WS2tcpip.h>
#include <winsock2.h>
#endif

namespace
{

volatile std::sig_atomic_t g_should_exit = 0;

void signal_handler(int)
{
    g_should_exit = 1;
}

#ifdef _WIN32
bool init_winsock()
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}
#endif

} // namespace

int main()
{
#ifndef _WIN32
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
#else
    if (!init_winsock()) {
        std::cerr << "winsock init failed\n";
        return 1;
    }
#endif

    yuan::app::RuntimeContext context;
    context.app_name = "webserver-example";
    context.run_mode = yuan::app::RunMode::single_thread;
    context.worker_threads = 1;

    yuan::app::Application application(context);
    if (!application.add_typed_service<yuan::server::DnsService>(
            "dns",
            std::make_shared<yuan::server::DnsService>(22002),
            "server.dns",
            1)) {
        std::cerr << "failed to register dns service\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    yuan::app::Bootstrap bootstrap(application);
    if (!bootstrap.run()) {
        std::cerr << "application bootstrap failed\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    const auto snapshot = bootstrap.supervisor_snapshot();

    LOG_INFO("application '{}' started with {} service(s), role={}, supervisor_state={}, worker_index={}, is_worker_process={}, owns_runtime={}",
             application.context().app_name,
             application.services().size(),
             yuan::app::to_string(bootstrap.process_role()),
             yuan::app::to_string(snapshot.state),
             application.context().worker_index,
             application.context().is_worker_process,
             bootstrap.owns_runtime());
    LOG_INFO("runtime identity: worker_index={}, is_worker_process={}",
             application.context().worker_index,
             application.context().is_worker_process);
    LOG_INFO("supervisor snapshot: reason={}, running_workers={}, recovering_workers={}, suppressed_workers={}, failed_workers={}, total_restarts={}, shutdown_started={}",
             yuan::app::to_string(snapshot.reason),
             snapshot.running_workers,
             snapshot.recovering_workers,
             snapshot.suppressed_workers,
             snapshot.failed_workers,
             snapshot.total_restarts,
             snapshot.shutdown_started);

    while (!g_should_exit) {
        bootstrap.poll_workers();
        if (bootstrap.process_role() == yuan::app::ProcessRole::supervisor &&
            (bootstrap.has_failed_workers() ||
             (!bootstrap.has_running_workers() && !bootstrap.has_recovering_workers()))) {
            if (bootstrap.has_failed_workers()) {
                LOG_WARN("supervisor detected worker failure");
            } else {
                LOG_WARN("all worker processes have exited and no worker is recovering");
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    bootstrap.shutdown();

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
