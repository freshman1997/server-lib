#ifndef __YUAN_APP_BOOTSTRAP_H__
#define __YUAN_APP_BOOTSTRAP_H__

#include "application.h"
#include "endpoint_manager.h"
#include "native_platform.h"
#include "worker_plan.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace yuan::app
{

enum class ProcessRole
{
    standalone,
    supervisor,
    worker,
};

const char *to_string(ProcessRole role) noexcept;

enum class SupervisorState
{
    idle,
    starting,
    running,
    recovering,
    degraded,
    stopping,
    stopped,
};

const char *to_string(SupervisorState state) noexcept;

enum class SupervisorReason
{
    none,
    standalone_start,
    spawning_initial_workers,
    initial_workers_started,
    worker_restarted,
    scheduled_worker_restart,
    waiting_for_more_due_restarts,
    restart_window_limit_reached,
    restart_limit_without_recovery_window,
    supervisor_circuit_opened,
    supervisor_circuit_recovered,
    worker_service_index_out_of_range,
    restart_spawn_failed,
    worker_failure_fail_fast,
    shutdown_requested,
    shutdown_complete,
};

const char *to_string(SupervisorReason reason) noexcept;

struct WorkerProcessInfo
{
    std::intptr_t pid = -1;
    std::string service_name;
    std::size_t worker_index = 0;
    std::size_t restart_count = 0;
    std::size_t restart_attempts_in_window = 0;
    std::uint64_t restart_window_started_at_ms = 0;
    std::uint64_t restart_due_at_ms = 0;
    bool running = false;
    bool pending_restart = false;
    bool restart_suppressed = false;
    bool supervisor_circuit_open = false;
    std::uint64_t supervisor_circuit_reset_at_ms = 0;
    int exit_status = 0;
    bool exited_normally = false;
};

struct SupervisorSnapshot
{
    SupervisorState state = SupervisorState::idle;
    SupervisorReason reason = SupervisorReason::none;
    std::size_t running_workers = 0;
    std::size_t recovering_workers = 0;
    std::size_t suppressed_workers = 0;
    std::size_t failed_workers = 0;
    std::size_t total_restarts = 0;
    bool circuit_open = false;
    std::uint64_t circuit_reset_at_ms = 0;
    bool shutdown_started = false;
};

class Bootstrap
{
public:
    explicit Bootstrap(Application& application);
    ~Bootstrap();

    bool run();
    void shutdown();
    void poll_workers();
    ProcessRole process_role() const noexcept;
    bool owns_runtime() const noexcept;
    bool has_running_workers() const noexcept;
    bool has_recovering_workers() const noexcept;
    bool has_failed_workers() const noexcept;
    SupervisorState supervisor_state() const noexcept;
    SupervisorReason supervisor_reason() const noexcept;
    SupervisorSnapshot supervisor_snapshot() const noexcept;
    const std::vector<WorkerProcessInfo>& worker_processes() const noexcept;

private:
    struct InProcessWorker;

    bool run_multi_process();
    bool run_worker_plan_multi_process();
    bool run_in_process_worker_plan();
    bool start_worker_process(const ServiceEntry &entry, std::size_t worker_index, std::size_t worker_count, WorkerProcessInfo *worker_info = nullptr);
    bool run_local_service_process(const ServiceEntry &entry, std::size_t worker_index, std::size_t worker_count);
    bool start_worker_process(const WorkerPlan &worker, WorkerProcessInfo *worker_info = nullptr);
    bool run_local_worker_process(const WorkerPlan &worker);
    bool start_in_process_worker(const WorkerPlan &worker);
    void shutdown_in_process_workers();
    void update_in_process_worker_failures();
    void start_due_worker_restarts();
    void set_supervisor_state(SupervisorState state, SupervisorReason reason = SupervisorReason::none);
    void reap_worker_processes(bool block_until_exit);
    void shutdown_multi_process();
    Application& application_;
    ProcessRole process_role_ = ProcessRole::standalone;
    std::vector<std::string> local_service_names_;
    bool worker_failure_detected_ = false;
    bool supervisor_shutdown_started_ = false;
    bool supervisor_circuit_open_ = false;
    std::uint64_t supervisor_failure_window_started_at_ms_ = 0;
    std::size_t supervisor_failures_in_window_ = 0;
    std::uint64_t supervisor_circuit_reset_at_ms_ = 0;
    SupervisorState supervisor_state_ = SupervisorState::idle;
    SupervisorReason supervisor_reason_ = SupervisorReason::none;
    std::unique_ptr<Application> local_worker_application_;
    std::vector<std::unique_ptr<InProcessWorker>> in_process_workers_;
    std::vector<WorkerPlan> worker_plans_;
    EndpointPlan endpoint_plan_;
#ifndef _WIN32
    std::vector<WorkerProcessInfo> worker_processes_;
#endif
    std::unique_ptr<NativePlatformGuard> native_platform_guard_;
};

} // namespace yuan::app

#endif
