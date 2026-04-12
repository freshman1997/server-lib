#include "application.h"
#include "bootstrap.h"
#include "plugin_host_service.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace
{

std::atomic<bool> g_running{true};

void signal_handler(int)
{
    g_running.store(false);
}

std::string resolve_plugin_path(int argc, char **argv)
{
    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        return argv[1];
    }

    if (argc > 0 && argv[0] && argv[0][0] != '\0') {
        const auto exe_path = std::filesystem::absolute(argv[0]).parent_path();
        const auto sibling_plugins = exe_path.parent_path() / "plugins";
        if (std::filesystem::exists(sibling_plugins)) {
            return sibling_plugins.string();
        }
    }

    return (std::filesystem::current_path() / "plugins").string();
}

#ifdef _WIN32
class WinsockGuard
{
public:
    bool init()
    {
        WSADATA wsa;
        initialized_ = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        return initialized_;
    }

    ~WinsockGuard()
    {
        if (initialized_) {
            WSACleanup();
        }
    }

private:
    bool initialized_ = false;
};
#endif

} // namespace

int main(int argc, char **argv)
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

#ifdef _WIN32
    WinsockGuard winsock;
    if (!winsock.init()) {
        std::cerr << "winsock init failed\n";
        return 1;
    }
#endif

    yuan::app::RuntimeContext context;
    context.app_name = "plugin-test";

    yuan::app::Application application(context);

    auto pluginHost = std::make_shared<yuan::app::PluginHostService>(resolve_plugin_path(argc, argv));
    if (!pluginHost->add_plugin("HelloWorld")) {
        std::cerr << "failed to register HelloWorld plugin\n";
        return 1;
    }

    if (!application.add_typed_service<yuan::app::PluginHostService>("plugins", pluginHost, "host.plugins", 1)) {
        std::cerr << "failed to register plugin host service\n";
        return 1;
    }

    yuan::app::Bootstrap bootstrap(application);
    if (!bootstrap.run()) {
        std::cerr << "plugin application bootstrap failed\n";
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

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (g_running.load()) {
        bootstrap.poll_workers();
        if (bootstrap.process_role() == yuan::app::ProcessRole::supervisor &&
            (bootstrap.has_failed_workers() ||
             (!bootstrap.has_running_workers() && !bootstrap.has_recovering_workers()))) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    bootstrap.shutdown();
    return 0;
}
