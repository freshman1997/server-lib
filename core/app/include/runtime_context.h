#ifndef __YUAN_APP_RUNTIME_CONTEXT_H__
#define __YUAN_APP_RUNTIME_CONTEXT_H__

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

    struct RuntimeContext
    {
        std::string app_name = "webserver";
        RunMode run_mode = RunMode::single_thread;
        std::size_t worker_threads = 1;
        std::size_t worker_index = 0;
        bool is_worker_process = false;
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

} // namespace yuan::app

#endif
