#ifndef __YUAN_APP_RUNTIME_CONTEXT_H__
#define __YUAN_APP_RUNTIME_CONTEXT_H__

#include <cstddef>
#include <memory>
#include <string>

namespace yuan::eventbus
{
    class EventBus;
    class EventTypeRegistry;
}

namespace yuan::net
{
    class NetworkRuntime;
}

namespace yuan::app
{

    class ServiceRegistry;

    enum class RunMode {
        single_thread,
        multi_thread,
        multi_process,
    };

    enum class WorkerProcessMode {
        in_process,
        process_per_worker,
    };

    struct RuntimeWorkerConfig
    {
        std::size_t worker_count = 0;
        WorkerProcessMode process_mode = WorkerProcessMode::in_process;
        bool restart_failed_workers = true;
    };

    struct RuntimeContext
    {
        std::string app_name = "webserver";
        RunMode run_mode = RunMode::single_thread;
        std::size_t worker_threads = 1;
        std::size_t worker_index = 0;
        std::size_t runtime_worker_count = 1;
        std::string active_service_name;
        std::size_t service_index = 0;
        std::size_t service_instance_index = 0;
        std::size_t service_instance_count = 1;
        bool listener_reuse_port = false;
        bool is_worker_process = false;
        RuntimeWorkerConfig runtime_workers;
        bool restart_failed_workers = true;
        std::size_t max_worker_restarts = 1;
        std::size_t worker_restart_backoff_ms = 500;
        std::size_t worker_restart_window_ms = 5000;
        std::size_t supervisor_failure_threshold = 3;
        std::size_t supervisor_failure_window_ms = 10000;
        std::size_t supervisor_circuit_backoff_ms = 3000;
        std::shared_ptr<eventbus::EventBus> event_bus;
        std::shared_ptr<ServiceRegistry> service_registry;
        std::shared_ptr<eventbus::EventTypeRegistry> event_type_registry;
        net::NetworkRuntime *shared_runtime = nullptr;
    };

    using RuntimeContextPtr = std::shared_ptr<RuntimeContext>;

    std::size_t normalized_worker_count(std::size_t worker_count) noexcept;
    void normalize_runtime_context(RuntimeContext &context);

} // namespace yuan::app

#endif
