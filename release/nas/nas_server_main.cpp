#include "application.h"
#include "bootstrap.h"
#include "nas/nas_service.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace
{
std::atomic_bool g_running{ true };

void signal_handler(int)
{
    g_running.store(false, std::memory_order_relaxed);
}

std::string read_env_string(const char *name, const std::string &default_value = {})
{
    const char *raw = std::getenv(name);
    return raw ? std::string(raw) : default_value;
}

std::filesystem::path default_config_path()
{
    const auto env_path = read_env_string("YUAN_NAS_CONFIG", "");
    if (!env_path.empty()) {
        return std::filesystem::path(env_path);
    }

    if (std::filesystem::exists(std::filesystem::path("release/nas/config.json"))) {
        return std::filesystem::path("release/nas/config.json");
    }

    return std::filesystem::path("config.json");
}

void print_usage(const char *program)
{
    std::cout
        << "release_nas_server\n"
        << "usage:\n"
        << "  " << program << " [--config <file>]\n"
        << "  " << program << " <config.json>\n\n"
        << "options:\n"
        << "  -f, --config <file>      Read NAS config JSON\n"
        << "      --version            Print version\n"
        << "  -h, --help               Show this help\n\n"
        << "env overrides:\n"
        << "  YUAN_NAS_CONFIG\n";
}
} // namespace

int main(int argc, char **argv)
{
#ifndef _WIN32
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
#endif

    std::filesystem::path config_path = default_config_path();

    for (int i = 1; i < argc; ++i) {
        const std::string opt = argv[i];
        auto need_value = [&](const std::string &name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << '\n';
                return {};
            }
            return argv[++i];
        };

        if (opt == "-h" || opt == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (opt == "--version") {
            std::cout << "release_nas_server 1.0.0\n";
            return 0;
        }
        if (opt == "-f" || opt == "--config") {
            const auto value = need_value(opt);
            if (value.empty()) {
                return 2;
            }
            config_path = value;
            continue;
        }
        if (!opt.empty() && opt.front() == '-') {
            std::cerr << "unknown option: " << opt << '\n';
            print_usage(argv[0]);
            return 2;
        }
        config_path = opt;
    }

    auto loaded = yuan::server::load_nas_service_config(config_path);
    if (!loaded) {
        std::cerr << "failed to load nas config: " << config_path << '\n';
        return 1;
    }

    yuan::app::RuntimeContext context;
    context.app_name = "release-nas";
    context.run_mode = yuan::app::RunMode::single_thread;
    context.worker_threads = 1;

    yuan::app::Application application(context);
    auto service = std::make_shared<yuan::server::NasService>(std::move(*loaded));
    if (!application.add_typed_service<yuan::server::NasService>(
            "nas",
            service,
            "server.nas",
            1)) {
        std::cerr << "failed to register nas service\n";
        return 1;
    }

    yuan::app::Bootstrap bootstrap(application);
    if (!bootstrap.run()) {
        std::cerr << "failed to start nas service\n";
        return 1;
    }

    std::cout << "release_nas_server started with config " << config_path << '\n';

    while (g_running.load(std::memory_order_relaxed)) {
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
