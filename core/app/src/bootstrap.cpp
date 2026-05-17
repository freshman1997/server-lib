#include "bootstrap.h"

#include "app_events.h"
#include "endpoint_manager.h"
#include "eventbus/event_bus.h"
#include "logger.h"
#include "net/runtime/network_runtime.h"
#include "runtime_plan.h"
#include "base/time.h"

#include <chrono>
#include <exception>
#include <thread>

#ifndef _WIN32
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace yuan::app
{

namespace
{

std::string worker_service_summary(const WorkerPlan &worker)
{
    std::string summary;
    for (const auto &instance : worker.service_instances) {
        if (!instance.definition) {
            continue;
        }
        if (!summary.empty()) {
            summary += ",";
        }
        summary += instance.definition->descriptor.name;
        summary += "[";
        summary += std::to_string(instance.service_instance_index);
        summary += "/";
        summary += std::to_string(instance.service_instance_count);
        summary += "]";
    }
    return summary.empty() ? "<empty>" : summary;
}

std::uint64_t now_ms()
{
    return yuan::base::time::steady_now_ms();
}

} // namespace

#ifndef _WIN32
namespace
{

volatile std::sig_atomic_t g_worker_should_exit = 0;

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
    event.runtime_worker_count = context.runtime_worker_count == 0
        ? context.worker_threads
        : context.runtime_worker_count;
    event.worker_index = worker.worker_index;
    event.is_worker_process = false;
    event.active_service_name = context.active_service_name;
    event.service_index = context.service_index;
    event.service_instance_index = context.service_instance_index;
    event.service_instance_count = context.service_instance_count == 0
        ? 1
        : context.service_instance_count;
    event.listener_reuse_port = context.listener_reuse_port;
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
    event.runtime_worker_count = context.runtime_worker_count == 0
        ? context.worker_threads
        : context.runtime_worker_count;
    event.worker_index = context.worker_index;
    event.is_worker_process = context.is_worker_process;
    event.active_service_name = context.active_service_name;
    event.service_index = context.service_index;
    event.service_instance_index = context.service_instance_index;
    event.service_instance_count = context.service_instance_count == 0
        ? 1
        : context.service_instance_count;
    event.listener_reuse_port = context.listener_reuse_port;
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

struct Bootstrap::InProcessWorker
{
    WorkerPlan plan;
    std::unique_ptr<net::NetworkRuntime> runtime;
    std::unique_ptr<Application> application;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable ready_cv;
    std::string error;
    std::size_t restart_count = 0;
    std::size_t restart_attempts_in_window = 0;
    std::uint64_t restart_window_started_at_ms = 0;
    std::uint64_t restart_due_at_ms = 0;
    bool pending_restart = false;
    bool restart_suppressed = false;
    bool supervisor_circuit_open = false;
    std::uint64_t supervisor_circuit_reset_at_ms = 0;
    std::atomic_bool ready{ false };
    std::atomic_bool started{ false };
    std::atomic_bool running{ false };
    std::atomic_bool failed{ false };
    std::atomic_bool stopping{ false };
};

Bootstrap::Bootstrap(Application& application)
    : application_(application),
      native_platform_guard_(std::make_unique<NativePlatformGuard>())
{
}

Bootstrap::~Bootstrap()
{
    shutdown();
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

    if (plan.run_mode == RunMode::multi_thread && !application_.service_definitions().empty()) {
        return run_in_process_worker_plan();
    }

    process_role_ = ProcessRole::standalone;
    set_supervisor_state(SupervisorState::idle, SupervisorReason::standalone_start);
    return application_.start();
}

void Bootstrap::shutdown()
{
    if (!in_process_workers_.empty()) {
        shutdown_in_process_workers();
        return;
    }

    if (process_role_ == ProcessRole::standalone) {
        application_.stop();
        return;
    }

    shutdown_multi_process();
}

void Bootstrap::poll_workers()
{
    update_in_process_worker_failures();

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
    for (const auto &worker : in_process_workers_) {
        if (worker && worker->running.load(std::memory_order_acquire)) {
            return true;
        }
    }

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
    for (const auto &worker : in_process_workers_) {
        if (worker && worker->pending_restart) {
            return true;
        }
    }

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
    for (const auto &worker : in_process_workers_) {
        if (worker && worker->failed.load(std::memory_order_acquire) &&
            !worker->stopping.load(std::memory_order_acquire) &&
            !worker->pending_restart) {
            return true;
        }
    }
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
    for (const auto &worker : in_process_workers_) {
        if (!worker) {
            continue;
        }
        if (worker->running.load(std::memory_order_acquire)) {
            ++snapshot.running_workers;
        }
        if (worker->pending_restart) {
            ++snapshot.recovering_workers;
        }
        if (worker->restart_suppressed) {
            ++snapshot.suppressed_workers;
        }
        if (worker->failed.load(std::memory_order_acquire)) {
            ++snapshot.failed_workers;
        }
        snapshot.total_restarts += worker->restart_count;
        snapshot.circuit_open = snapshot.circuit_open || worker->supervisor_circuit_open;
        snapshot.circuit_reset_at_ms = (std::max)(snapshot.circuit_reset_at_ms, worker->supervisor_circuit_reset_at_ms);
    }
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

bool Bootstrap::run_local_worker_process(const WorkerPlan &worker)
{
    if (worker.service_instances.empty()) {
        LOG_WARN("worker process {} has no service instances", worker.worker_index);
        return false;
    }

    auto context = application_.context();
    context.worker_threads = worker.worker_count;
    context.runtime_worker_count = worker.worker_count;
    context.runtime_workers.worker_count = worker.worker_count;
    context.worker_index = worker.worker_index;
    context.is_worker_process = true;

    local_worker_application_ = std::make_unique<Application>(context);
    local_service_names_.clear();

    for (const auto &instance : worker.service_instances) {
        if (!instance.definition || !instance.definition->factory) {
            LOG_ERROR("worker process {} has an invalid service definition", worker.worker_index);
            return false;
        }

        auto service = instance.definition->create_instance();
        if (!service) {
            LOG_ERROR(
                "worker process {} failed to create service '{}'",
                worker.worker_index,
                instance.definition->descriptor.name);
            return false;
        }

        ServiceInstanceRuntime runtime;
        runtime.service_index = instance.service_index;
        runtime.service_instance_index = instance.service_instance_index;
        runtime.service_instance_count = instance.service_instance_count;
        runtime.listener_reuse_port = service_instance_requires_reuse_port(endpoint_plan_, instance);

        if (!local_worker_application_->add_service_instance(instance.definition->descriptor, std::move(service), runtime)) {
            LOG_ERROR(
                "worker process {} failed to register service '{}'",
                worker.worker_index,
                instance.definition->descriptor.name);
            return false;
        }

        local_service_names_.push_back(instance.definition->descriptor.name);
    }

    if (!local_worker_application_->start()) {
        LOG_ERROR("worker process {} failed to start services '{}'", worker.worker_index, worker_service_summary(worker));
        return false;
    }

    process_role_ = ProcessRole::worker;
    LOG_INFO("worker process {} started service plan '{}'", worker.worker_index, worker_service_summary(worker));
    return true;
}

bool Bootstrap::start_in_process_worker(const WorkerPlan &worker)
{
    auto worker_state = std::make_unique<InProcessWorker>();
    worker_state->plan = worker;
    worker_state->restart_window_started_at_ms = now_ms();
    auto *state = worker_state.get();
    const auto base_context = application_.context();
    const auto endpoint_plan = endpoint_plan_;

    state->thread = std::thread([state, worker, base_context, endpoint_plan]() {
        auto mark_ready = [state]() {
            state->ready.store(true, std::memory_order_release);
            state->ready_cv.notify_all();
        };
        auto fail = [state, &mark_ready](std::string error) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->error = std::move(error);
            }
            state->failed.store(true, std::memory_order_release);
            state->running.store(false, std::memory_order_release);
            mark_ready();
        };

        try
        {
            state->runtime = std::make_unique<net::NetworkRuntime>();

            auto context = base_context;
            context.worker_threads = worker.worker_count;
            context.runtime_worker_count = worker.worker_count;
            context.runtime_workers.worker_count = worker.worker_count;
            context.worker_index = worker.worker_index;
            context.is_worker_process = false;
            context.shared_runtime = state->runtime.get();
            context.service_registry = std::make_shared<ServiceRegistry>();

            state->application = std::make_unique<Application>(context);

            for (const auto &instance : worker.service_instances) {
                if (!instance.definition || !instance.definition->factory) {
                    fail("invalid service definition in worker plan");
                    return;
                }

                auto service = instance.definition->create_instance();
                if (!service) {
                    fail("failed to create service '" + instance.definition->descriptor.name + "'");
                    return;
                }

                ServiceInstanceRuntime runtime;
                runtime.service_index = instance.service_index;
                runtime.service_instance_index = instance.service_instance_index;
                runtime.service_instance_count = instance.service_instance_count;
                runtime.listener_reuse_port = service_instance_requires_reuse_port(endpoint_plan, instance);

                if (!state->application->add_service_instance(instance.definition->descriptor, std::move(service), runtime)) {
                    fail("failed to register service '" + instance.definition->descriptor.name + "'");
                    return;
                }
            }

            if (!state->application->start()) {
                fail("worker-local application failed to start");
                return;
            }

            state->started.store(true, std::memory_order_release);
            state->running.store(true, std::memory_order_release);
            mark_ready();
            LOG_INFO("in-process runtime worker {} started service plan '{}'",
                     worker.worker_index,
                     worker_service_summary(worker));

            (void)state->runtime->run();

            state->running.store(false, std::memory_order_release);
            if (!state->stopping.load(std::memory_order_acquire)) {
                if (state->application) {
                    state->application->stop();
                }
                fail("runtime worker exited unexpectedly");
            }
        }
        catch (const std::exception &e)
        {
            fail(e.what());
        }
        catch (...)
        {
            fail("unknown runtime worker exception");
        }
    });

    in_process_workers_.push_back(std::move(worker_state));

    std::unique_lock<std::mutex> lock(state->mutex);
    const bool ready = state->ready_cv.wait_for(lock, std::chrono::seconds(10), [state]() {
        return state->ready.load(std::memory_order_acquire);
    });
    if (!ready) {
        state->stopping.store(true, std::memory_order_release);
        if (state->runtime) {
            state->runtime->stop();
        }
        state->error = "runtime worker start timed out";
        return false;
    }

    return state->started.load(std::memory_order_acquire) ||
           !state->failed.load(std::memory_order_acquire);
}

bool Bootstrap::run_in_process_worker_plan()
{
    const auto &definitions = application_.service_definitions();
    if (definitions.empty()) {
        LOG_WARN("in-process worker plan requested without service definitions");
        return false;
    }

    auto runtime_config = application_.context().runtime_workers;
    if (runtime_config.worker_count == 0) {
        runtime_config.worker_count = application_.context().worker_threads == 0 ? 1 : application_.context().worker_threads;
    }

    shutdown_in_process_workers();

    worker_plans_ = build_worker_plan(runtime_config, definitions);
    if (worker_plans_.empty()) {
        LOG_WARN("in-process worker plan produced no workers");
        return false;
    }
    endpoint_plan_ = EndpointManager::build_plan(worker_plans_);
    if (!endpoint_plan_.valid()) {
        for (const auto &diagnostic : endpoint_plan_.diagnostics) {
            LOG_ERROR("in-process worker endpoint plan invalid: {}", diagnostic);
        }
        return false;
    }

    worker_failure_detected_ = false;
    supervisor_shutdown_started_ = false;

    auto supervisor_context = application_.context();
    supervisor_context.worker_threads = worker_plans_.size();
    supervisor_context.runtime_worker_count = worker_plans_.size();
    supervisor_context.runtime_workers.worker_count = worker_plans_.size();
    supervisor_context.worker_index = 0;
    supervisor_context.is_worker_process = false;
    supervisor_context.shared_runtime = nullptr;
    if (!supervisor_context.event_bus) {
        supervisor_context.event_bus = std::make_shared<yuan::eventbus::EventBus>();
    }
    application_.set_context(supervisor_context);

    process_role_ = ProcessRole::standalone;
    set_supervisor_state(SupervisorState::starting, SupervisorReason::spawning_initial_workers);

    for (const auto &worker : worker_plans_) {
        if (!start_in_process_worker(worker)) {
            LOG_ERROR("failed to start in-process runtime worker {}: {}",
                      worker.worker_index,
                      in_process_workers_.empty() || !in_process_workers_.back()
                          ? std::string("unknown")
                          : in_process_workers_.back()->error);
            shutdown_in_process_workers();
            return false;
        }
    }

    set_supervisor_state(SupervisorState::running, SupervisorReason::initial_workers_started);
    return true;
}

void Bootstrap::update_in_process_worker_failures()
{
    if (in_process_workers_.empty()) {
        return;
    }

    const auto context = application_.context();
    auto current_ms = now_ms();

    if (supervisor_circuit_open_) {
        if (supervisor_circuit_reset_at_ms_ == 0 || current_ms < supervisor_circuit_reset_at_ms_) {
            return;
        }

        supervisor_circuit_open_ = false;
        supervisor_failure_window_started_at_ms_ = current_ms;
        supervisor_failures_in_window_ = 0;
        supervisor_circuit_reset_at_ms_ = 0;
        for (auto &worker : in_process_workers_) {
            if (!worker) {
                continue;
            }
            worker->supervisor_circuit_open = false;
            worker->supervisor_circuit_reset_at_ms = 0;
            if (worker->pending_restart) {
                worker->restart_suppressed = false;
            }
        }
        set_supervisor_state(SupervisorState::recovering, SupervisorReason::supervisor_circuit_recovered);
    }

    const bool restart_enabled = context.restart_failed_workers && context.runtime_workers.restart_failed_workers;

    for (auto &worker : in_process_workers_) {
        if (!worker || worker->stopping.load(std::memory_order_acquire) ||
            !worker->failed.load(std::memory_order_acquire)) {
            continue;
        }

        if (worker->thread.joinable()) {
            worker->thread.join();
        }

        if (worker->pending_restart) {
            continue;
        }

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
            for (auto &candidate : in_process_workers_) {
                if (!candidate) {
                    continue;
                }
                candidate->supervisor_circuit_open = true;
                candidate->supervisor_circuit_reset_at_ms = supervisor_circuit_reset_at_ms_;
                if (candidate->pending_restart) {
                    candidate->restart_suppressed = true;
                    candidate->restart_due_at_ms = supervisor_circuit_reset_at_ms_;
                }
            }
            worker->pending_restart = restart_enabled;
            worker->restart_suppressed = restart_enabled;
            worker->restart_due_at_ms = restart_enabled ? supervisor_circuit_reset_at_ms_ : 0;
            worker->supervisor_circuit_open = true;
            worker->supervisor_circuit_reset_at_ms = supervisor_circuit_reset_at_ms_;
            if (!restart_enabled) {
                worker_failure_detected_ = true;
            }
            set_supervisor_state(SupervisorState::degraded, SupervisorReason::supervisor_circuit_opened);
            LOG_WARN(
                "in-process runtime worker {} opened supervisor circuit after {} failures; reset_at_ms={}",
                worker->plan.worker_index,
                supervisor_failures_in_window_,
                supervisor_circuit_reset_at_ms_);
            continue;
        }

        if (!restart_enabled) {
            worker_failure_detected_ = true;
            set_supervisor_state(SupervisorState::degraded, SupervisorReason::worker_failure_fail_fast);
            LOG_WARN(
                "in-process runtime worker {} failed and automatic restart is disabled: {}",
                worker->plan.worker_index,
                worker->error);
            continue;
        }

        if (worker->restart_window_started_at_ms == 0 ||
            (context.worker_restart_window_ms > 0 &&
             current_ms - worker->restart_window_started_at_ms > context.worker_restart_window_ms)) {
            worker->restart_window_started_at_ms = current_ms;
            worker->restart_attempts_in_window = 0;
        }

        if (worker->restart_attempts_in_window >= context.max_worker_restarts) {
            if (context.worker_restart_window_ms > 0) {
                worker->pending_restart = true;
                worker->restart_suppressed = true;
                worker->restart_due_at_ms = worker->restart_window_started_at_ms + context.worker_restart_window_ms;
                worker->supervisor_circuit_open = supervisor_circuit_open_;
                worker->supervisor_circuit_reset_at_ms = supervisor_circuit_reset_at_ms_;
                set_supervisor_state(SupervisorState::degraded, SupervisorReason::restart_window_limit_reached);
                LOG_WARN(
                    "in-process runtime worker {} reached restart limit ({}) and is suppressed until {} ms",
                    worker->plan.worker_index,
                    context.max_worker_restarts,
                    worker->restart_due_at_ms);
                continue;
            }

            worker_failure_detected_ = true;
            set_supervisor_state(SupervisorState::degraded, SupervisorReason::restart_limit_without_recovery_window);
            LOG_WARN(
                "in-process runtime worker {} reached restart limit ({}) without a recovery window",
                worker->plan.worker_index,
                context.max_worker_restarts);
            continue;
        }

        worker->restart_count += 1;
        worker->restart_attempts_in_window += 1;
        worker->pending_restart = true;
        worker->restart_suppressed = false;
        worker->restart_due_at_ms = current_ms + context.worker_restart_backoff_ms;
        worker->supervisor_circuit_open = supervisor_circuit_open_;
        worker->supervisor_circuit_reset_at_ms = supervisor_circuit_reset_at_ms_;
        set_supervisor_state(SupervisorState::recovering, SupervisorReason::scheduled_worker_restart);
        LOG_WARN(
            "scheduled restart for in-process runtime worker {} plan '{}' (restart_count={}, attempts_in_window={}, backoff_ms={}, error='{}')",
            worker->plan.worker_index,
            worker_service_summary(worker->plan),
            worker->restart_count,
            worker->restart_attempts_in_window,
            context.worker_restart_backoff_ms,
            worker->error);
    }

    current_ms = now_ms();
    if (supervisor_circuit_open_ && (supervisor_circuit_reset_at_ms_ == 0 || current_ms < supervisor_circuit_reset_at_ms_)) {
        return;
    }

    for (std::size_t index = 0; index < in_process_workers_.size(); ++index) {
        auto &worker = in_process_workers_[index];
        if (!worker || !worker->pending_restart || worker->restart_due_at_ms > current_ms) {
            continue;
        }

        if (worker->thread.joinable()) {
            worker->thread.join();
        }

        if (worker->restart_suppressed) {
            worker->restart_window_started_at_ms = current_ms;
            worker->restart_attempts_in_window = 0;
            worker->restart_suppressed = false;
        }

        auto plan = worker->plan;
        const auto restart_count = worker->restart_count;
        const auto restart_attempts_in_window = worker->restart_attempts_in_window;
        const auto restart_window_started_at_ms = worker->restart_window_started_at_ms;
        const auto supervisor_circuit_open = worker->supervisor_circuit_open;
        const auto supervisor_circuit_reset_at_ms = worker->supervisor_circuit_reset_at_ms;

        in_process_workers_.erase(in_process_workers_.begin() + static_cast<std::ptrdiff_t>(index));

        const bool restarted = start_in_process_worker(plan);
        auto *new_worker = in_process_workers_.empty() ? nullptr : in_process_workers_.back().get();
        if (new_worker) {
            new_worker->restart_count = restart_count;
            new_worker->restart_attempts_in_window = restart_attempts_in_window;
            new_worker->restart_window_started_at_ms = restart_window_started_at_ms == 0
                ? current_ms
                : restart_window_started_at_ms;
            new_worker->supervisor_circuit_open = supervisor_circuit_open;
            new_worker->supervisor_circuit_reset_at_ms = supervisor_circuit_reset_at_ms;
        }

        if (restarted && new_worker) {
            LOG_WARN(
                "restarted in-process runtime worker {} plan '{}' (restart_count={}, attempts_in_window={})",
                plan.worker_index,
                worker_service_summary(plan),
                restart_count,
                restart_attempts_in_window);

            bool any_pending_restart = false;
            for (const auto &candidate : in_process_workers_) {
                if (candidate && candidate->pending_restart) {
                    any_pending_restart = true;
                    break;
                }
            }
            if (!worker_failure_detected_) {
                set_supervisor_state(
                    any_pending_restart ? SupervisorState::recovering : SupervisorState::running,
                    any_pending_restart ? SupervisorReason::waiting_for_more_due_restarts : SupervisorReason::worker_restarted);
            }
            return;
        }

        if (new_worker) {
            new_worker->pending_restart = true;
            new_worker->restart_suppressed = false;
            new_worker->restart_due_at_ms =
                current_ms + (std::max)(context.worker_restart_backoff_ms, static_cast<std::size_t>(1000));
        }
        set_supervisor_state(SupervisorState::degraded, SupervisorReason::restart_spawn_failed);
        LOG_ERROR(
            "failed to restart in-process runtime worker {} plan '{}' and rescheduled recovery after {} ms",
            plan.worker_index,
            worker_service_summary(plan),
            (std::max)(context.worker_restart_backoff_ms, static_cast<std::size_t>(1000)));
        return;
    }
}

void Bootstrap::shutdown_in_process_workers()
{
    if (in_process_workers_.empty()) {
        return;
    }

    supervisor_shutdown_started_ = true;
    set_supervisor_state(SupervisorState::stopping, SupervisorReason::shutdown_requested);

    for (auto &worker : in_process_workers_) {
        if (!worker) {
            continue;
        }
        worker->stopping.store(true, std::memory_order_release);
        if (worker->application) {
            worker->application->stop();
        }
        if (worker->runtime) {
            worker->runtime->stop();
        }
    }

    for (auto &worker : in_process_workers_) {
        if (worker && worker->thread.joinable()) {
            worker->thread.join();
        }
    }

    in_process_workers_.clear();
    worker_plans_.clear();
    endpoint_plan_ = {};
    local_service_names_.clear();
    set_supervisor_state(SupervisorState::stopped, SupervisorReason::shutdown_complete);
}

bool Bootstrap::start_worker_process(const WorkerPlan &worker, WorkerProcessInfo *worker_info)
{
#ifdef _WIN32
    (void)worker;
    (void)worker_info;
    return false;
#else
    const auto pid = ::fork();
    if (pid < 0) {
        LOG_ERROR("failed to fork worker process for service plan '{}'", worker_service_summary(worker));
        return false;
    }

    if (pid == 0) {
        const auto local_worker = worker;
        worker_processes_.clear();
        worker_plans_.clear();
        g_worker_should_exit = 0;
        std::signal(SIGINT, worker_signal_handler);
        std::signal(SIGTERM, worker_signal_handler);
        std::signal(SIGPIPE, SIG_IGN);

        const bool ok = run_local_worker_process(local_worker);
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
        worker_info->service_name = worker_service_summary(worker);
        worker_info->worker_index = worker.worker_index;
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

    LOG_INFO(
        "supervisor started worker pid={} for service plan '{}' (worker_index={})",
        pid,
        worker_service_summary(worker),
        worker.worker_index);
    return true;
#endif
}

bool Bootstrap::run_worker_plan_multi_process()
{
#ifdef _WIN32
    LOG_WARN("multi-process mode is not implemented on Windows/MinGW");
    return false;
#else
    const auto &definitions = application_.service_definitions();
    if (definitions.empty()) {
        LOG_WARN("worker-plan multi-process mode requested without service definitions");
        return false;
    }

    auto runtime_config = application_.context().runtime_workers;
    if (runtime_config.worker_count == 0) {
        runtime_config.worker_count = application_.context().worker_threads == 0 ? 1 : application_.context().worker_threads;
    }

    worker_plans_ = build_worker_plan(runtime_config, definitions);
    if (worker_plans_.empty()) {
        LOG_WARN("worker-plan multi-process mode produced no workers");
        return false;
    }
    endpoint_plan_ = EndpointManager::build_plan(worker_plans_);
    if (!endpoint_plan_.valid()) {
        for (const auto &diagnostic : endpoint_plan_.diagnostics) {
            LOG_ERROR("worker-plan multi-process endpoint plan invalid: {}", diagnostic);
        }
        return false;
    }

    worker_processes_.clear();
    local_service_names_.clear();
    worker_failure_detected_ = false;
    supervisor_shutdown_started_ = false;
    process_role_ = ProcessRole::supervisor;
    set_supervisor_state(SupervisorState::starting, SupervisorReason::spawning_initial_workers);

    auto supervisor_context = application_.context();
    supervisor_context.worker_threads = worker_plans_.size();
    supervisor_context.runtime_worker_count = worker_plans_.size();
    supervisor_context.runtime_workers.worker_count = worker_plans_.size();
    supervisor_context.worker_index = 0;
    supervisor_context.is_worker_process = false;
    application_.set_context(supervisor_context);
    ensure_event_bus(application_);

    for (const auto &worker : worker_plans_) {
        WorkerProcessInfo worker_info;
        worker_info.worker_index = worker.worker_index;
        if (!start_worker_process(worker, &worker_info)) {
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

bool Bootstrap::run_multi_process()
{
#ifdef _WIN32
    LOG_WARN("multi-process mode is not implemented on Windows/MinGW");
    return false;
#else
    if (!application_.service_definitions().empty()) {
        return run_worker_plan_multi_process();
    }

    const auto &services = application_.services();
    if (services.empty()) {
        LOG_WARN("multi-process mode requested without any registered service");
        return false;
    }

    worker_processes_.clear();
    worker_plans_.clear();
    endpoint_plan_ = {};
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

        if (!worker_plans_.empty()) {
            if (worker.worker_index >= worker_plans_.size()) {
                worker_failure_detected_ = true;
                set_supervisor_state(SupervisorState::degraded, SupervisorReason::worker_service_index_out_of_range);
                continue;
            }
        } else {
            const auto &services = application_.services();
            if (worker.worker_index >= services.size()) {
                worker_failure_detected_ = true;
                set_supervisor_state(SupervisorState::degraded, SupervisorReason::worker_service_index_out_of_range);
                continue;
            }
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

        auto restart_info = worker;
        bool restarted = false;
        if (!worker_plans_.empty()) {
            if (worker.worker_index >= worker_plans_.size()) {
                worker.pending_restart = false;
                worker_failure_detected_ = true;
                set_supervisor_state(SupervisorState::degraded, SupervisorReason::worker_service_index_out_of_range);
                continue;
            }
            restarted = start_worker_process(worker_plans_[worker.worker_index], &restart_info);
        } else {
            const auto &services = application_.services();
            if (worker.worker_index >= services.size()) {
                worker.pending_restart = false;
                worker_failure_detected_ = true;
                set_supervisor_state(SupervisorState::degraded, SupervisorReason::worker_service_index_out_of_range);
                continue;
            }
            restarted = start_worker_process(
                services[worker.worker_index],
                worker.worker_index,
                application_.context().worker_threads,
                &restart_info);
        }

        if (restarted) {
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
