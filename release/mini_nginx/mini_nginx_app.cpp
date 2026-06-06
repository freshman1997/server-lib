#include "mini_nginx_app.h"
#include "mini_nginx_common.h"

namespace mini_nginx
{
    namespace
    {
        std::atomic_bool g_running{true};
        std::atomic_bool g_reload_requested{false};

        void terminate_handler(int)
        {
            g_running.store(false, std::memory_order_relaxed);
        }

        void reload_handler(int)
        {
            g_reload_requested.store(true, std::memory_order_relaxed);
        }
    }

    int run(int argc, char **argv)
    {
        std::signal(SIGINT, terminate_handler);
        std::signal(SIGTERM, terminate_handler);
#ifndef _WIN32
        std::signal(SIGHUP, reload_handler);
        std::signal(SIGPIPE, SIG_IGN);
#endif

        CliOptions cli;
        if (!parse_cli(argc, argv, cli)) {
            print_usage(argv[0]);
            return 1;
        }
        if (cli.help) {
            print_usage(argv[0]);
            return 0;
        }

        const std::string config_path = cli.config_path;

        MiniNginxConfig cfg;
        if (!load_and_apply_config(config_path, cfg)) {
            return 1;
        }

        apply_env_overrides(cfg);
        if (!validate_loaded_config(cfg)) {
            return 1;
        }

        if (cli.test_config) {
            std::cout << "configuration file " << config_path << " test is successful\n"
                      << "listen: " << cfg.listen_port << '\n'
                      << "routes: " << cfg.routes.size() << '\n'
                      << "static mounts: " << cfg.static_mounts.size() << '\n';
            return 0;
        }

        yuan::app::RuntimeContext context;
        context.app_name = "mini-nginx";
        const int requested_worker_processes = cfg.worker_processes < 1 ? 1 : cfg.worker_processes;
        int effective_worker_processes = requested_worker_processes;
        if (requested_worker_processes > 1) {
#ifdef _WIN32
            std::cerr << "worker_processes>1 requires POSIX multi-process mode; falling back to one worker on Windows\n";
            effective_worker_processes = 1;
#else
            context.run_mode = yuan::app::RunMode::multi_process;
            context.worker_threads = static_cast<std::size_t>(requested_worker_processes);
            context.runtime_workers.worker_count = static_cast<std::size_t>(requested_worker_processes);
            context.runtime_workers.process_mode = yuan::app::WorkerProcessMode::process_per_worker;
#endif
        }

        yuan::app::Application application(context);
        yuan::server::HttpService *local_http_service = nullptr;

        yuan::app::ServiceDescriptor http_descriptor;
        http_descriptor.name = "http";
        http_descriptor.type_name = "yuan::server::HttpService";
        http_descriptor.contract_id = "server.http";
        http_descriptor.contract_version = 1;
        http_descriptor.placement.mode = effective_worker_processes > 1
                                             ? yuan::app::PlacementMode::all_workers
                                             : yuan::app::PlacementMode::singleton;
        http_descriptor.placement.instances = static_cast<std::size_t>(effective_worker_processes);
        http_descriptor.endpoints.push_back(yuan::app::ServiceEndpoint{
            "http",
            "0.0.0.0",
            cfg.listen_port,
            "tcp"});

        if (!application.add_service(http_descriptor, [&cfg, &local_http_service]() {
                auto service = create_http_service(cfg);
                local_http_service = service.get();
                return service;
            })) {
            std::cerr << "failed to register http service\n";
            return 1;
        }

        yuan::app::Bootstrap bootstrap(application);
        if (!bootstrap.run()) {
            std::cerr << "failed to start mini_nginx service\n";
            return 1;
        }

        auto *proxy = local_http_service ? local_http_service->server().ensure_proxy() : nullptr;
        if (!proxy && bootstrap.process_role() != yuan::app::ProcessRole::supervisor) {
            std::cerr << "http proxy module is unavailable\n";
            bootstrap.shutdown();
            return 1;
        }

        std::cout << "mini_nginx listening on 0.0.0.0:" << cfg.listen_port << " using config " << config_path << '\n';
        std::cout << "routes loaded: " << cfg.routes.size() << '\n';
        std::cout << "static mounts loaded: " << cfg.static_mounts.size() << '\n';
        std::cout << "worker processes: " << effective_worker_processes << '\n';
        const bool affinity_mode =
            cfg.server_config.listen_options.scheduling_mode == yuan::net::ListenSchedulingMode::affinity;
#ifdef _WIN32
        std::cout << "listen backend: " << (cfg.server_config.listen_options.use_iocp ? "iocp" : "default")
                  << ", scheduling: " << (affinity_mode ? "affinity" : "throughput")
                  << ", shards: " << cfg.server_config.listen_options.shard_count
                  << ", iocp workers: " << cfg.server_config.listen_options.iocp_worker_count
                  << ", completion batch: " << cfg.server_config.listen_options.iocp_completion_batch_size
                  << '\n';
#else
        std::cout << "listen backend: default"
                  << ", scheduling: " << (affinity_mode ? "affinity" : "throughput")
                  << ", shards: " << cfg.server_config.listen_options.shard_count
                  << '\n';
#endif
        std::cout << "reload check interval: " << cfg.reload_check_interval_ms << " ms\n";

        std::error_code ec;
        auto last_config_write_time = std::filesystem::last_write_time(config_path, ec);
        if (ec) {
            last_config_write_time = std::filesystem::file_time_type::min();
        }

        auto last_reload_check = std::chrono::steady_clock::now();

        while (g_running.load(std::memory_order_relaxed)) {
            bootstrap.poll_workers();

            const auto now = std::chrono::steady_clock::now();
            if (cfg.reload_check_interval_ms > 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_reload_check).count() >= cfg.reload_check_interval_ms) {
                last_reload_check = now;
                std::error_code check_ec;
                const auto current_write = std::filesystem::last_write_time(config_path, check_ec);
                if (!check_ec && current_write != last_config_write_time) {
                    last_config_write_time = current_write;
                    g_reload_requested.store(true, std::memory_order_relaxed);
                }
            }

            if (g_reload_requested.exchange(false, std::memory_order_relaxed)) {
                if (proxy && local_http_service) {
                    (void)reload_routes(proxy, config_path, local_http_service->server(), cfg);
                } else {
                    std::cerr << "reload note: multi-process route reload requires restart\n";
                }
            }

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
}
