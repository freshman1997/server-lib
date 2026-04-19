#include "bootstrap.h"

#include "app_events.h"
#include "eventbus/event_bus.h"
#include "logger.h"
#include "runtime_plan.h"
#include "base/time.h"

#include <thread>

#ifndef _WIN32
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace yuan::app
{

#ifndef _WIN32
namespace
{

volatile std::sig_atomic_t g_worker_should_exit = 0;

std::uint64_t now_ms()
{
    return yuan::base::time::steady_now_ms();
}

void worker_signal_handler(int)
{
    g_worker_should_exit = 1;
}

void ensure_event_bus(Application &application)
{
    auto context = application.context();
    if (!context.event_bus) {
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();
        application.set_context(std::move(context));
    }
}

WorkerProcessEvent make_worker_event(const RuntimeContext &context, const WorkerProcessInfo &worker)
{
    WorkerProcessEvent event;
    event.app_name = context.app_name;
    event.run_mode = context.run_mode;
    event.worker_threads = context.worker_threads;
    event.worker_index = worker.worker_index;
    event.is_worker_process = false;
    event.service_name = worker.service_name;
    event.pid = worker.pid;
    event.restart_count = worker.restart_count;
    event.restart_attempts_in_window = worker.restart_attempts_in_window;
    event.restart_pending = worker.pending_restart;
    event.restart_suppressed = worker.restart_suppressed;
    event.restart_failed_workers = context.restart_failed_workers;
    event.max_worker_restarts = context.max_worker_restarts;
    event.worker_restart_backoff_ms = context.worker_restart_backoff_ms;
    event.worker_restart_window_ms = context.worker_restart_window_ms;
    event.supervisor_circuit_open = worker.supervisor_circuit_open;
    event.supervisor_circuit_reset_at_ms = worker.supervisor_circuit_reset_at_ms;
    event.exit_status = worker.exit_status;
    event.exited_normally = worker.exited_normally;
    return event;
}

SupervisorStateEvent make_supervisor_state_event(
    const RuntimeContext &context,
    const SupervisorState state,
    const SupervisorReason reason,
    const std::vector<WorkerProcessInfo> &workers,
    const bool shutdown_started)
{
    SupervisorStateEvent event;
    event.app_name = context.app_name;
    event.run_mode = context.run_mode;
    event.worker_threads = context.worker_threads;
    event.worker_index = context.worker_index;
    event.is_worker_process = context.is_worker_process;
    event.state = to_string(state);
    event.reason_code = to_string(reason);
    event.reason = event.reason_code;
    event.shutdown_started = shutdown_started;
    for (const auto &worker : workers) {
        if (worker.running) {
            ++event.running_workers;
        }
        if (worker.pending_restart) {
            ++event.recovering_workers;
        }
        if (worker.restart_suppressed) {
            ++event.suppressed_workers;
        }
        if (!worker.running && !worker.exited_normally && worker.pid > 0) {
            ++event.failed_workers;
        }
        event.total_restarts += worker.restart_count;
        event.circuit_open = event.circuit_open || worker.supervisor_circuit_open;
        event.circuit_reset_at_ms = (std::max)(event.circuit_reset_at_ms, worker.supervisor_circuit_reset_at_ms);
    }
    return event;
}

} // namespace
#endif

const char *to_string(const ProcessRole role) noexcept
{
    switch (role) {
    case ProcessRole::standalone:
        return "standalone";
    case ProcessRole::supervisor:
        return "supervisor";
    case ProcessRole::worker:
        return "worker";
    default:
        return "unknown";
    }
}

const char *to_string(const SupervisorState state) noexcept
{
    switch (state) {
    case SupervisorState::idle:
        return "idle";
    case SupervisorState::starting:
        return "starting";
    case SupervisorState::running:
        return "running";
    case SupervisorState::recovering:
        return "recovering";
    case SupervisorState::degraded:
        return "degraded";
    case SupervisorState::stopping:
        return "stopping";
    case SupervisorState::stopped:
        return "stopped";
    default:
        return "unknown";
    }
}

const char *to_string(const SupervisorReason reason) noexcept
{
    switch (reason) {
    case SupervisorReason::none:
        return "none";
    case SupervisorReason::standalone_start:
        return "standalone_start";
    case SupervisorReason::spawning_initial_workers:
        return "spawning_initial_workers";
    case SupervisorReason::initial_workers_started:
        return "initial_workers_started";
    case SupervisorReason::worker_restarted:
        return "worker_restarted";
    case SupervisorReason::scheduled_worker_restart:
        return "scheduled_worker_restart";
    case SupervisorReason::waiting_for_more_due_restarts:
        return "waiting_for_more_due_restarts";
    case SupervisorReason::restart_window_limit_reached:
        return "restart_window_limit_reached";
    case SupervisorReason::restart_limit_without_recovery_window:
        return "restart_limit_without_recovery_window";
    case SupervisorReason::supervisor_circuit_opened:
        return "supervisor_circuit_opened";
    case SupervisorReason::supervisor_circuit_recovered:
        return "supervisor_circuit_recovered";
    case SupervisorReason::worker_service_index_out_of_range:
        return "worker_service_index_out_of_range";
    case SupervisorReason::restart_spawn_failed:
        return "restart_spawn_failed";
    case SupervisorReason::worker_failure_fail_fast:
        return "worker_failure_fail_fast";
    case SupervisorReason::shutdown_requested:
        return "shutdown_requested";
    case SupervisorReason::shutdown_complete:
        return "shutdown_complete";
    default:
        return "unknown";
    }
}

Bootstrap::Bootstrap(Application& application)
    : application_(application),
      native_platform_guard_(std::make_unique<NativePlatformGuard>())
{
}

bool Bootstrap::run()
{
    if (native_platform_guard_ && !native_platform_guard_->ok()) {
        LOG_ERROR("failed to initialize native platform");
        return false;
    }

    const auto plan = derive_runtime_plan(application_.context());
    if (!plan.implemented) {
        LOG_WARN(
            "runtime plan is not implemented yet (run_mode={}, event_loop_mode={}): {}",
            static_cast<int>(plan.run_mode),
            to_string(plan.event_loop_mode),
            plan.note);
        return false;
    }

    if (plan.run_mode == RunMode::multi_process) {
        return run_multi_process();
    }

    process_role_ = ProcessRole::standalone;
    set_supervisor_state(SupervisorState::idle, SupervisorReason::standalone_start);
    return application_.start();
}

void Bootstrap::shutdown()
{
    if (process_role_ == ProcessRole::standalone) {
        application_.stop();
        return;
    }

    shutdown_multi_process();
}

void Bootstrap::poll_workers()
{
#ifndef _WIN32
    if (process_role_ == ProcessRole::supervisor) {
        reap_worker_processes(false);
        start_due_worker_restarts();
        if (worker_failure_detected_ && has_running_workers() && !supervisor_shutdown_started_) {
            supervisor_shutdown_started_ = true;
            set_supervisor_state(SupervisorState::degraded, SupervisorReason::worker_failure_fail_fast);
            LOG_WARN("worker failure detected; supervisor is stopping remaining workers");
            for (const auto &worker : worker_processes_) {
                if (worker.running && worker.pid > 0) {
                    ::kill(static_cast<pid_t>(worker.pid), SIGTERM);
                }
            }
        }
    }
#endif
}

ProcessRole Bootstrap::process_role() const noexcept
{
    return process_role_;
}

bool Bootstrap::owns_runtime() const noexcept
{
    return process_role_ != ProcessRole::supervisor;
}

bool Bootstrap::has_running_workers() const noexcept
{
#ifndef _WIN32
    for (const auto &worker : worker_processes_) {
        if (worker.running) {
            return true;
        }
    }
#endif
    return false;
}

bool Bootstrap::has_recovering_workers() const noexcept
{
#ifndef _WIN32
    for (const auto &worker : worker_processes_) {
        if (worker.pending_restart) {
            return true;
        }
    }
#endif
    return false;
}

bool Bootstrap::has_failed_workers() const noexcept
{
    return worker_failure_detected_;
}

SupervisorState Bootstrap::supervisor_state() const noexcept
{
    return supervisor_state_;
}

SupervisorReason Bootstrap::supervisor_reason() const noexcept
{
    return supervisor_reason_;
}

SupervisorSnapshot Bootstrap::supervisor_snapshot() const noexcept
{
    SupervisorSnapshot snapshot;
    snapshot.state = supervisor_state_;
    snapshot.reason = supervisor_reason_;
    snapshot.shutdown_started = supervisor_shutdown_started_;
#ifndef _WIN32
    for (const auto &worker : worker_processes_) {
        if (worker.running) {
            ++snapshot.running_workers;
        }
        if (worker.pending_restart) {
            ++snapshot.recovering_workers;
        }
        if (worker.restart_suppressed) {
            ++snapshot.suppressed_workers;
        }
        if (!worker.running && !worker.exited_normally && worker.pid > 0) {
            ++snapshot.failed_workers;
        }
        snapshot.total_restarts += worker.restart_count;
        snapshot.circuit_open = snapshot.circuit_open || worker.supervisor_circuit_open;
        snapshot.circuit_reset_at_ms = (std::max)(snapshot.circuit_reset_at_ms, worker.supervisor_circuit_reset_at_ms);
    }
#endif
    return snapshot;
}

const std::vector<WorkerProcessInfo>& Bootstrap::worker_processes() const noexcept
{
#ifndef _WIN32
    return worker_processes_;
#else
    static const std::vector<WorkerProcessInfo> empty_workers;
    return empty_workers;
#endif
}

void Bootstrap::set_supervisor_state(const SupervisorState state, const SupervisorReason reason)
{
    if (supervisor_state_ == state && supervisor_reason_ == reason) {
        return;
    }

    supervisor_state_ = state;
    supervisor_reason_ = reason;
#ifndef _WIN32
    if (process_role_ == ProcessRole::supervisor && application_.context().event_bus) {
        application_.context().event_bus->publish(
            events::supervisor_state_changed,
            make_supervisor_state_event(application_.context(), supervisor_state_, supervisor_reason_, worker_processes_, supervisor_shutdown_started_));
    }
#endif
}

bool Bootstrap::run_local_service_process(
    const ServiceEntry &entry,
    const std::size_t worker_index,
    const std::size_t worker_count)
{
    if (!entry.service) {
        return false;
    }

    auto context = application_.context();
    context.worker_threads = worker_count;
    context.worker_index = worker_index;
    context.is_worker_process = true;
    local_worker_application_ = std::make_unique<Application>(context);
    if (!local_worker_application_->add_service(entry.descriptor, entry.service)) {
        LOG_ERROR("worker process {} failed to register service '{}'", worker_index, entry.descriptor.name);
        return false;
    }

    if (!local_worker_application_->start()) {
        LOG_ERROR("worker process {} failed to start service '{}'", worker_index, entry.descriptor.name);
        return false;
    }

    local_service_names_.clear();
    local_service_names_.push_back(entry.descriptor.name);
    process_role_ = ProcessRole::worker;
    LOG_INFO("worker process {} started service '{}'", worker_index, entry.descriptor.name);
    return true;
}

bool Bootstrap::start_worker_process(
    const ServiceEntry &entry,
    const std::size_t worker_index,
    const std::size_t worker_count,
    WorkerProcessInfo *worker_info)
{
#ifdef _WIN32
    (void)entry;
    (void)worker_index;
    (void)worker_count;
    (void)worker_info;
    return false;
#else
    const auto pid = ::fork();
    if (pid < 0) {
        LOG_ERROR("failed to fork worker process for service '{}'", entry.descriptor.name);
        return false;
    }

    if (pid == 0) {
        worker_processes_.clear();
        g_worker_should_exit = 0;
        std::signal(SIGINT, worker_signal_handler);
        std::signal(SIGTERM, worker_signal_handler);
        std::signal(SIGPIPE, SIG_IGN);

        const bool ok = run_local_service_process(entry, worker_index, worker_count);
        if (!ok) {
            std::_Exit(1);
        }

        while (!g_worker_should_exit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        shutdown_multi_process();
        std::_Exit(0);
    }

    if (worker_info) {
        worker_info->pid = static_cast<std::intptr_t>(pid);
        worker_info->service_name = entry.descriptor.name;
        worker_info->worker_index = worker_index;
        worker_info->running = true;
        worker_info->pending_restart = false;
        worker_info->restart_suppressed = false;
        worker_info->exit_status = 0;
        worker_info->exited_normally = false;
        if (worker_info->restart_window_started_at_ms == 0) {
            worker_info->restart_window_started_at_ms = now_ms();
        }
        worker_info->restart_due_at_ms = 0;
    }

    LOG_INFO("supervisor started worker pid={} for service '{}' (worker_index={})", pid, entry.descriptor.name, worker_index);
    return true;
#endif
}

bool Bootstrap::run_multi_process()
{
#ifdef _WIN32
    LOG_WARN("multi-process mode is not implemented on Windows/MinGW");
    return false;
#else
    const auto &services = application_.services();
    if (services.empty()) {
        LOG_WARN("multi-process mode requested without any registered service");
        return false;
    }

    worker_processes_.clear();
    local_service_names_.clear();
    worker_failure_detected_ = false;
    supervisor_shutdown_started_ = false;
    const auto worker_count = services.size();
    process_role_ = ProcessRole::supervisor;
    set_supervisor_state(SupervisorState::starting, SupervisorReason::spawning_initial_workers);

    auto supervisor_context = application_.context();
    supervisor_context.worker_threads = worker_count;
    supervisor_context.worker_index = 0;
    supervisor_context.is_worker_process = false;
    application_.set_context(supervisor_context);
    ensure_event_bus(application_);

    for (std::size_t i = 0; i < services.size(); ++i) {
        const auto &entry = services[i];
        WorkerProcessInfo worker_info;
        worker_info.worker_index = i;
        if (!start_worker_process(entry, i, worker_count, &worker_info)) {
            shutdown_multi_process();
            return false;
        }
        worker_processes_.push_back(std::move(worker_info));
        application_.context().event_bus->publish(events::worker_started, make_worker_event(application_.context(), worker_processes_.back()));
    }

    set_supervisor_state(SupervisorState::running, SupervisorReason::initial_workers_started);
    return true;
#endif
}

void Bootstrap::reap_worker_processes(const bool block_until_exit)
{
#ifndef _WIN32
    for (auto &worker : worker_processes_) {
        if (!worker.running || worker.pid <= 0) {
            continue;
        }

        int status = 0;
        const auto options = block_until_exit ? 0 : WNOHANG;
        const auto result = ::waitpid(static_cast<pid_t>(worker.pid), &status, options);
        if (result == 0 || result < 0) {
            continue;
        }

        worker.running = false;
        worker.pending_restart = false;
        worker.restart_suppressed = false;
        worker.exit_status = status;
        worker.exited_normally = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        const bool abnormal_exit = !worker.exited_normally;

        if (WIFEXITED(status)) {
            LOG_INFO(
                "worker pid={} for service '{}' exited with code {}",
                worker.pid,
                worker.service_name,
                WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            LOG_WARN(
                "worker pid={} for service '{}' terminated by signal {}",
                worker.pid,
                worker.service_name,
                WTERMSIG(status));
        } else {
            LOG_WARN(
                "worker pid={} for service '{}' exited with status {}",
                worker.pid,
                worker.service_name,
                status);
        }

        application_.context().event_bus->publish(events::worker_exited, make_worker_event(application_.context(), worker));

        if (!abnormal_exit || supervisor_shutdown_started_) {
            continue;
        }

        const auto context = application_.context();
        const auto current_ms = now_ms();
        if (supervisor_failure_window_started_at_ms_ == 0 ||
            (context.supervisor_failure_window_ms > 0 &&
             current_ms - supervisor_failure_window_started_at_ms_ > context.supervisor_failure_window_ms)) {
            supervisor_failure_window_started_at_ms_ = current_ms;
            supervisor_failures_in_window_ = 0;
        }
        ++supervisor_failures_in_window_;

        if (context.supervisor_failure_threshold > 0 &&
            supervisor_failures_in_window_ >= context.supervisor_failure_threshold) {
            supervisor_circuit_open_ = true;
            supervisor_circuit_reset_at_ms_ = current_ms + context.supervisor_circuit_backoff_ms;
            for (auto &candidate : worker_processes_) {
                candidate.supervisor_circuit_open = true;
                candidate.supervisor_circuit_reset_at_ms = supervisor_circuit_reset_at_ms_;
                if (candidate.pending_restart) {
                    candidate.restart_suppressed = true;
                    candidate.restart_due_at_ms = supervisor_circuit_reset_at_ms_;
                }
            }
            worker.supervisor_circuit_open = true;
            worker.supervisor_circuit_reset_at_ms = supervisor_circuit_reset_at_ms_;
            set_supervisor_state(SupervisorState::degraded, SupervisorReason::supervisor_circuit_opened);
            application_.context().event_bus->publish(
                events::worker_restart_limit_reached,
                make_worker_event(application_.context(), worker));
            continue;
        }

        if (!context.restart_failed_workers) {
            continue;
        }

        if (worker.restart_window_started_at_ms == 0 ||
            (context.worker_restart_window_ms > 0 &&
             current_ms - worker.restart_window_started_at_ms > context.worker_restart_window_ms)) {
            worker.restart_window_started_at_ms = current_ms;
            worker.restart_attempts_in_window = 0;
        }

        if (worker.restart_attempts_in_window >= context.max_worker_restarts) {
            if (context.worker_restart_window_ms > 0) {
                set_supervisor_state(SupervisorState::degraded, SupervisorReason::restart_window_limit_reached);
                worker.pending_restart = true;
                worker.restart_suppressed = true;
                worker.restart_due_at_ms = worker.restart_window_started_at_ms + context.worker_restart_window_ms;
                worker.supervisor_circuit_open = supervisor_circuit_open_;
                worker.supervisor_circuit_reset_at_ms = supervisor_circuit_reset_at_ms_;
                LOG_WARN(
                    "worker '{}' reached restart limit ({}) and is suppressed until restart window resets at {} ms",
                    worker.service_name,
                    context.max_worker_restarts,
                    worker.restart_due_at_ms);
                application_.context().event_bus->publish(
                    events::worker_restart_limit_reached,
                    make_worker_event(application_.context(), worker));
                continue;
            }

            set_supervisor_state(SupervisorState::degraded, SupervisorReason::restart_limit_without_recovery_window);
            worker_failure_detected_ = true;
            LOG_WARN(
                "worker '{}' reached restart limit ({}) without a recovery window",
                worker.service_name,
                context.max_worker_restarts);
            application_.context().event_bus->publish(
                events::worker_restart_limit_reached,
                make_worker_event(application_.context(), worker));
            continue;
        }

        const auto &services = application_.services();
        if (worker.worker_index >= services.size()) {
            worker_failure_detected_ = true;
            set_supervisor_state(SupervisorState::degraded, SupervisorReason::worker_service_index_out_of_range);
            continue;
        }

        worker.restart_count += 1;
        worker.restart_attempts_in_window += 1;
        worker.pending_restart = true;
        worker.restart_suppressed = false;
        worker.restart_due_at_ms = current_ms + context.worker_restart_backoff_ms;
        worker.supervisor_circuit_open = supervisor_circuit_open_;
        worker.supervisor_circuit_reset_at_ms = supervisor_circuit_reset_at_ms_;
        set_supervisor_state(SupervisorState::recovering, SupervisorReason::scheduled_worker_restart);
        LOG_WARN(
            "supervisor scheduled restart for service '{}' (restart_count={}, attempts_in_window={}, backoff_ms={})",
            worker.service_name,
            worker.restart_count,
            worker.restart_attempts_in_window,
            context.worker_restart_backoff_ms);
    }
#else
    (void)block_until_exit;
#endif
}

void Bootstrap::start_due_worker_restarts()
{
#ifndef _WIN32
    const auto current_ms = now_ms();
    const auto &services = application_.services();

    if (supervisor_circuit_open_) {
        if (supervisor_circuit_reset_at_ms_ == 0 || current_ms < supervisor_circuit_reset_at_ms_) {
            return;
        }

        supervisor_circuit_open_ = false;
        supervisor_failure_window_started_at_ms_ = current_ms;
        supervisor_failures_in_window_ = 0;
        supervisor_circuit_reset_at_ms_ = 0;
        for (auto &worker : worker_processes_) {
            worker.supervisor_circuit_open = false;
            worker.supervisor_circuit_reset_at_ms = 0;
            if (worker.pending_restart) {
                worker.restart_suppressed = false;
            }
        }
        set_supervisor_state(SupervisorState::recovering, SupervisorReason::supervisor_circuit_recovered);
    }

    for (auto &worker : worker_processes_) {
        if (!worker.pending_restart || worker.restart_due_at_ms > current_ms) {
            continue;
        }

        if (worker.restart_suppressed) {
            worker.restart_window_started_at_ms = current_ms;
            worker.restart_attempts_in_window = 0;
            worker.restart_suppressed = false;
        }
        worker.supervisor_circuit_open = supervisor_circuit_open_;
        worker.supervisor_circuit_reset_at_ms = supervisor_circuit_reset_at_ms_;

        if (worker.worker_index >= services.size()) {
            worker.pending_restart = false;
            worker_failure_detected_ = true;
            set_supervisor_state(SupervisorState::degraded, SupervisorReason::worker_service_index_out_of_range);
            continue;
        }

        auto restart_info = worker;
        if (start_worker_process(services[worker.worker_index], worker.worker_index, application_.context().worker_threads, &restart_info)) {
            worker = std::move(restart_info);
            LOG_WARN(
                "supervisor restarted worker for service '{}' (restart_count={}, attempts_in_window={})",
                worker.service_name,
                worker.restart_count,
                worker.restart_attempts_in_window);
            application_.context().event_bus->publish(events::worker_restarted, make_worker_event(application_.context(), worker));

            bool any_pending_restart = false;
            for (const auto &candidate : worker_processes_) {
                if (candidate.pending_restart) {
                    any_pending_restart = true;
                    break;
                }
            }
            if (!worker_failure_detected_) {
                set_supervisor_state(
                    any_pending_restart ? SupervisorState::recovering : SupervisorState::running,
                    any_pending_restart ? SupervisorReason::waiting_for_more_due_restarts : SupervisorReason::worker_restarted);
            }
            continue;
        }

        worker.pending_restart = true;
        worker.restart_suppressed = false;
        worker.restart_due_at_ms = current_ms + std::max<std::size_t>(application_.context().worker_restart_backoff_ms, 1000);
        worker.supervisor_circuit_open = supervisor_circuit_open_;
        worker.supervisor_circuit_reset_at_ms = supervisor_circuit_reset_at_ms_;
        set_supervisor_state(SupervisorState::degraded, SupervisorReason::restart_spawn_failed);
        LOG_ERROR(
            "supervisor failed to restart worker for service '{}' and rescheduled recovery after {} ms",
            worker.service_name,
            std::max<std::size_t>(application_.context().worker_restart_backoff_ms, 1000));
        application_.context().event_bus->publish(
            events::worker_restart_limit_reached,
            make_worker_event(application_.context(), worker));
    }
#endif
}

void Bootstrap::shutdown_multi_process()
{
    if (process_role_ == ProcessRole::worker) {
        if (local_worker_application_) {
            local_worker_application_->stop();
            local_worker_application_.reset();
        } else {
            const auto &services = application_.services();
            for (auto it = local_service_names_.rbegin(); it != local_service_names_.rend(); ++it) {
                for (const auto &entry : services) {
                    if (entry.descriptor.name == *it && entry.service) {
                        entry.service->stop();
                        break;
                    }
                }
            }
        }
        local_service_names_.clear();
        return;
    }

#ifndef _WIN32
    if (process_role_ == ProcessRole::supervisor) {
        supervisor_shutdown_started_ = true;
        set_supervisor_state(SupervisorState::stopping, SupervisorReason::shutdown_requested);
        for (const auto &worker : worker_processes_) {
            if (worker.running && worker.pid > 0) {
                ::kill(static_cast<pid_t>(worker.pid), SIGTERM);
            }
        }

        const auto deadline = yuan::base::time::steady_now_ms() + 2000;
        while (has_running_workers() && yuan::base::time::steady_now_ms() < deadline) {
            reap_worker_processes(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        for (const auto &worker : worker_processes_) {
            if (worker.running && worker.pid > 0) {
                ::kill(static_cast<pid_t>(worker.pid), SIGKILL);
            }
        }

        while (has_running_workers()) {
            reap_worker_processes(true);
        }

        set_supervisor_state(SupervisorState::stopped, SupervisorReason::shutdown_complete);
    }
#endif
}

} // namespace yuan::app
