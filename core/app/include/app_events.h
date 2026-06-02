#ifndef __YUAN_APP_APP_EVENTS_H__
#define __YUAN_APP_APP_EVENTS_H__

#include "runtime_context.h"

#include <cstdint>
#include <string>

namespace yuan::app
{

namespace events
{
    inline constexpr const char *application_initialized = "app.initialized";
    inline constexpr const char *application_started = "app.started";
    inline constexpr const char *application_stopping = "app.stopping";
    inline constexpr const char *service_initialized = "app.service.initialized";
    inline constexpr const char *service_started = "app.service.started";
    inline constexpr const char *service_stopped = "app.service.stopped";
    inline constexpr const char *worker_started = "app.worker.started";
    inline constexpr const char *worker_exited = "app.worker.exited";
    inline constexpr const char *worker_restarted = "app.worker.restarted";
    inline constexpr const char *worker_restart_limit_reached = "app.worker.restart_limit_reached";
    inline constexpr const char *supervisor_state_changed = "app.supervisor.state_changed";
}

struct ApplicationEvent
{
    std::string app_name;
    RunMode run_mode = RunMode::single_thread;
    std::size_t worker_threads = 1;
    std::size_t runtime_worker_count = 1;
    std::size_t worker_index = 0;
    bool is_worker_process = false;
    std::string active_service_name;
    std::size_t service_index = 0;
    std::size_t service_instance_index = 0;
    std::size_t service_instance_count = 1;
    bool listener_reuse_port = false;
};

struct ServiceEvent : public ApplicationEvent
{
    std::string service_name;
};

struct WorkerProcessEvent : public ApplicationEvent
{
    std::string service_name;
    std::intptr_t pid = -1;
    std::size_t restart_count = 0;
    std::size_t restart_attempts_in_window = 0;
    bool restart_pending = false;
    bool restart_suppressed = false;
    bool restart_failed_workers = false;
    std::size_t max_worker_restarts = 0;
    std::size_t worker_restart_backoff_ms = 0;
    std::size_t worker_restart_window_ms = 0;
    bool supervisor_circuit_open = false;
    std::uint64_t supervisor_circuit_reset_at_ms = 0;
    int exit_status = 0;
    bool exited_normally = false;
};

struct SupervisorStateEvent : public ApplicationEvent
{
    std::string state;
    std::string reason_code;
    std::string reason;
    std::size_t running_workers = 0;
    std::size_t recovering_workers = 0;
    std::size_t suppressed_workers = 0;
    std::size_t failed_workers = 0;
    std::size_t total_restarts = 0;
    bool circuit_open = false;
    std::uint64_t circuit_reset_at_ms = 0;
    bool shutdown_started = false;
};

inline std::size_t normalized_runtime_worker_count(const RuntimeContext &context) noexcept
{
    return context.runtime_worker_count == 0
        ? context.worker_threads
        : context.runtime_worker_count;
}

inline std::size_t normalized_service_instance_count(const RuntimeContext &context) noexcept
{
    return context.service_instance_count == 0
        ? 1
        : context.service_instance_count;
}

inline void populate_application_event(ApplicationEvent &event, const RuntimeContext &context)
{
    event.app_name = context.app_name;
    event.run_mode = context.run_mode;
    event.worker_threads = context.worker_threads;
    event.runtime_worker_count = normalized_runtime_worker_count(context);
    event.worker_index = context.worker_index;
    event.is_worker_process = context.is_worker_process;
    event.active_service_name = context.active_service_name;
    event.service_index = context.service_index;
    event.service_instance_index = context.service_instance_index;
    event.service_instance_count = normalized_service_instance_count(context);
    event.listener_reuse_port = context.listener_reuse_port;
}

inline ApplicationEvent make_application_event(const RuntimeContext &context)
{
    ApplicationEvent event;
    populate_application_event(event, context);
    return event;
}

inline ServiceEvent make_service_event(const RuntimeContext &context, const std::string &service_name)
{
    ServiceEvent event;
    populate_application_event(event, context);
    event.service_name = service_name;
    return event;
}

} // namespace yuan::app

#endif
