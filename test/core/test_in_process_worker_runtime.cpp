#include "bootstrap.h"
#include "eventbus/event_bus.h"
#include "net/runtime/network_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    bool require(bool condition, const std::string &message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

    struct ContextSnapshot
    {
        std::size_t worker_index = 0;
        std::size_t runtime_worker_count = 0;
        std::size_t service_instance_index = 0;
        std::size_t service_instance_count = 0;
        bool shared_runtime = false;
        bool listener_reuse_port = false;
    };

    class RuntimeProbeService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        void set_runtime_context(const yuan::app::RuntimeContext &context) override
        {
            context_ = context;
        }

        bool init() override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            contexts_.push_back(ContextSnapshot{
                context_.worker_index,
                context_.runtime_worker_count,
                context_.service_instance_index,
                context_.service_instance_count,
                context_.shared_runtime != nullptr,
                context_.listener_reuse_port
            });
            return context_.shared_runtime != nullptr;
        }

        void start() override
        {
            ++started_;
            if (context_.shared_runtime) {
                context_.shared_runtime->dispatch([]() {
                    ++dispatched_;
                });
            }
        }

        void stop() override
        {
            ++stopped_;
        }

        static void reset()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            contexts_.clear();
            started_.store(0, std::memory_order_release);
            stopped_.store(0, std::memory_order_release);
            dispatched_.store(0, std::memory_order_release);
        }

        static std::vector<ContextSnapshot> contexts()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return contexts_;
        }

        static int started()
        {
            return started_.load(std::memory_order_acquire);
        }

        static int stopped()
        {
            return stopped_.load(std::memory_order_acquire);
        }

        static int dispatched()
        {
            return dispatched_.load(std::memory_order_acquire);
        }

    private:
        yuan::app::RuntimeContext context_;
        static std::mutex mutex_;
        static std::vector<ContextSnapshot> contexts_;
        static std::atomic_int started_;
        static std::atomic_int stopped_;
        static std::atomic_int dispatched_;
    };

    std::mutex RuntimeProbeService::mutex_;
    std::vector<ContextSnapshot> RuntimeProbeService::contexts_;
    std::atomic_int RuntimeProbeService::started_{ 0 };
    std::atomic_int RuntimeProbeService::stopped_{ 0 };
    std::atomic_int RuntimeProbeService::dispatched_{ 0 };

    class RestartProbeService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        void set_runtime_context(const yuan::app::RuntimeContext &context) override
        {
            context_ = context;
        }

        bool init() override
        {
            return context_.shared_runtime != nullptr;
        }

        void start() override
        {
            const int generation = ++started_;
            auto *runtime = context_.shared_runtime;
            if (!runtime) {
                return;
            }

            if (generation == 1) {
                runtime->dispatch([runtime]() {
                    runtime->stop();
                });
                return;
            }

            runtime->dispatch([]() {
                ++post_restart_dispatches_;
            });
        }

        void stop() override
        {
            ++stopped_;
        }

        static void reset()
        {
            started_.store(0, std::memory_order_release);
            stopped_.store(0, std::memory_order_release);
            post_restart_dispatches_.store(0, std::memory_order_release);
        }

        static int started()
        {
            return started_.load(std::memory_order_acquire);
        }

        static int stopped()
        {
            return stopped_.load(std::memory_order_acquire);
        }

        static int post_restart_dispatches()
        {
            return post_restart_dispatches_.load(std::memory_order_acquire);
        }

    private:
        yuan::app::RuntimeContext context_;
        static std::atomic_int started_;
        static std::atomic_int stopped_;
        static std::atomic_int post_restart_dispatches_;
    };

    std::atomic_int RestartProbeService::started_{ 0 };
    std::atomic_int RestartProbeService::stopped_{ 0 };
    std::atomic_int RestartProbeService::post_restart_dispatches_{ 0 };

    class RestartLimitProbeService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        void set_runtime_context(const yuan::app::RuntimeContext &context) override
        {
            context_ = context;
        }

        bool init() override
        {
            return context_.shared_runtime != nullptr;
        }

        void start() override
        {
            ++started_;
            auto *runtime = context_.shared_runtime;
            if (!runtime) {
                return;
            }

            runtime->dispatch([runtime]() {
                runtime->stop();
            });
        }

        void stop() override
        {
            ++stopped_;
        }

        static void reset()
        {
            started_.store(0, std::memory_order_release);
            stopped_.store(0, std::memory_order_release);
        }

        static int started()
        {
            return started_.load(std::memory_order_acquire);
        }

        static int stopped()
        {
            return stopped_.load(std::memory_order_acquire);
        }

    private:
        yuan::app::RuntimeContext context_;
        static std::atomic_int started_;
        static std::atomic_int stopped_;
    };

    std::atomic_int RestartLimitProbeService::started_{ 0 };
    std::atomic_int RestartLimitProbeService::stopped_{ 0 };

    bool wait_for_dispatched(int expected)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (RuntimeProbeService::dispatched() >= expected) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return RuntimeProbeService::dispatched() >= expected;
    }

    bool wait_for_restart(yuan::app::Bootstrap &bootstrap)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            bootstrap.poll_workers();
            const auto snapshot = bootstrap.supervisor_snapshot();
            if (RestartProbeService::started() >= 2 &&
                RestartProbeService::post_restart_dispatches() >= 1 &&
                snapshot.running_workers == 1 &&
                snapshot.failed_workers == 0 &&
                snapshot.total_restarts >= 1 &&
                !bootstrap.has_failed_workers()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    bool wait_for_restart_suppressed(yuan::app::Bootstrap &bootstrap)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            bootstrap.poll_workers();
            const auto snapshot = bootstrap.supervisor_snapshot();
            if (RestartLimitProbeService::started() >= 2 &&
                snapshot.running_workers == 0 &&
                snapshot.recovering_workers == 1 &&
                snapshot.suppressed_workers == 1 &&
                snapshot.total_restarts == 1) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    bool wait_for_supervisor_circuit_open(yuan::app::Bootstrap &bootstrap)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            bootstrap.poll_workers();
            const auto snapshot = bootstrap.supervisor_snapshot();
            if (RestartLimitProbeService::started() == 1 &&
                snapshot.circuit_open &&
                snapshot.recovering_workers == 1 &&
                snapshot.suppressed_workers == 1 &&
                bootstrap.supervisor_reason() == yuan::app::SupervisorReason::supervisor_circuit_opened) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    bool wait_for_supervisor_circuit_recovery_restart(yuan::app::Bootstrap &bootstrap)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            bootstrap.poll_workers();
            if (RestartLimitProbeService::started() >= 2) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
}

int main()
{
    {
        RuntimeProbeService::reset();

        yuan::app::RuntimeContext context;
        context.app_name = "in-process-worker-runtime-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 3;
        context.runtime_workers.worker_count = 3;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::Application app(context);

        yuan::app::ServiceDescriptor descriptor;
        descriptor.name = "probe";
        descriptor.type_name = "RuntimeProbeService";
        descriptor.contract_id = "test.runtime-probe";
        descriptor.contract_version = 1;
        descriptor.placement.mode = yuan::app::PlacementMode::all_workers;
        descriptor.endpoints.push_back(yuan::app::ServiceEndpoint{
            "probe",
            "127.0.0.1",
            49152,
            "tcp"
        });

        if (!require(app.add_service(descriptor, []() {
                return std::make_shared<RuntimeProbeService>();
            }), "factory service should register")) {
            return 1;
        }

        yuan::app::Bootstrap bootstrap(app);
        if (!require(bootstrap.run(), "bootstrap should start in-process runtime workers")) {
            return 1;
        }

        const auto snapshot = bootstrap.supervisor_snapshot();
        if (!require(snapshot.running_workers == 3,
                     "bootstrap snapshot should report three running in-process workers")) {
            return 1;
        }
        if (!require(RuntimeProbeService::started() == 3,
                     "all worker-local service instances should start")) {
            return 1;
        }
        if (!require(wait_for_dispatched(3),
                     "all worker-local runtimes should execute queued dispatch callbacks")) {
            return 1;
        }

        auto contexts = RuntimeProbeService::contexts();
        if (!require(contexts.size() == 3, "all worker-local service instances should receive context")) {
            return 1;
        }
        std::sort(contexts.begin(), contexts.end(), [](const auto &left, const auto &right) {
            return left.worker_index < right.worker_index;
        });
        for (std::size_t i = 0; i < contexts.size(); ++i) {
            const auto &ctx = contexts[i];
            if (!require(ctx.worker_index == i &&
                             ctx.runtime_worker_count == 3 &&
                             ctx.service_instance_index == i &&
                             ctx.service_instance_count == 3,
                         "worker-local service context should match WorkerPlan identity")) {
                return 1;
            }
            if (!require(ctx.shared_runtime, "worker-local service should receive shared runtime")) {
                return 1;
            }
            if (!require(ctx.listener_reuse_port, "replicated endpoint should request listener reuse")) {
                return 1;
            }
        }

        bootstrap.shutdown();
        if (!require(RuntimeProbeService::stopped() == 3,
                     "all worker-local service instances should stop during bootstrap shutdown")) {
            return 1;
        }
    }

    {
        RestartProbeService::reset();

        yuan::app::RuntimeContext context;
        context.app_name = "in-process-worker-restart-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 1;
        context.runtime_workers.worker_count = 1;
        context.restart_failed_workers = true;
        context.runtime_workers.restart_failed_workers = true;
        context.max_worker_restarts = 2;
        context.worker_restart_backoff_ms = 1;
        context.worker_restart_window_ms = 5000;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::Application app(context);

        yuan::app::ServiceDescriptor descriptor;
        descriptor.name = "restart-probe";
        descriptor.type_name = "RestartProbeService";
        descriptor.contract_id = "test.restart-probe";
        descriptor.contract_version = 1;
        descriptor.placement.mode = yuan::app::PlacementMode::all_workers;

        if (!require(app.add_service(descriptor, []() {
                return std::make_shared<RestartProbeService>();
            }), "restart probe factory service should register")) {
            return 1;
        }

        yuan::app::Bootstrap bootstrap(app);
        if (!require(bootstrap.run(), "bootstrap should start restart probe worker")) {
            return 1;
        }

        if (!require(wait_for_restart(bootstrap),
                     "bootstrap should restart failed in-process runtime worker")) {
            return 1;
        }

        const auto snapshot = bootstrap.supervisor_snapshot();
        if (!require(snapshot.running_workers == 1 &&
                         snapshot.failed_workers == 0 &&
                         snapshot.total_restarts >= 1,
                     "snapshot should report one healthy restarted in-process worker")) {
            return 1;
        }

        bootstrap.shutdown();
        if (!require(RestartProbeService::stopped() >= 2,
                     "restart probe service should stop both failed and restarted generations")) {
            return 1;
        }
    }

    {
        RestartLimitProbeService::reset();

        yuan::app::RuntimeContext context;
        context.app_name = "in-process-worker-restart-limit-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 1;
        context.runtime_workers.worker_count = 1;
        context.restart_failed_workers = true;
        context.runtime_workers.restart_failed_workers = true;
        context.max_worker_restarts = 1;
        context.worker_restart_backoff_ms = 1;
        context.worker_restart_window_ms = 10000;
        context.supervisor_failure_threshold = 0;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::Application app(context);

        yuan::app::ServiceDescriptor descriptor;
        descriptor.name = "restart-limit-probe";
        descriptor.type_name = "RestartLimitProbeService";
        descriptor.contract_id = "test.restart-limit-probe";
        descriptor.contract_version = 1;
        descriptor.placement.mode = yuan::app::PlacementMode::all_workers;

        if (!require(app.add_service(descriptor, []() {
                return std::make_shared<RestartLimitProbeService>();
            }), "restart limit probe factory service should register")) {
            return 1;
        }

        yuan::app::Bootstrap bootstrap(app);
        if (!require(bootstrap.run(), "bootstrap should start restart limit probe worker")) {
            return 1;
        }

        if (!require(wait_for_restart_suppressed(bootstrap),
                     "bootstrap should suppress in-process runtime worker after restart limit")) {
            return 1;
        }

        bootstrap.shutdown();
        if (!require(RestartLimitProbeService::stopped() >= 2,
                     "restart limit probe service should stop failed generations")) {
            return 1;
        }
    }

    {
        RestartLimitProbeService::reset();

        yuan::app::RuntimeContext context;
        context.app_name = "in-process-worker-circuit-breaker-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 1;
        context.runtime_workers.worker_count = 1;
        context.restart_failed_workers = true;
        context.runtime_workers.restart_failed_workers = true;
        context.max_worker_restarts = 5;
        context.worker_restart_backoff_ms = 1;
        context.worker_restart_window_ms = 10000;
        context.supervisor_failure_threshold = 1;
        context.supervisor_failure_window_ms = 10000;
        context.supervisor_circuit_backoff_ms = 50;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::Application app(context);

        yuan::app::ServiceDescriptor descriptor;
        descriptor.name = "circuit-breaker-probe";
        descriptor.type_name = "RestartLimitProbeService";
        descriptor.contract_id = "test.circuit-breaker-probe";
        descriptor.contract_version = 1;
        descriptor.placement.mode = yuan::app::PlacementMode::all_workers;

        if (!require(app.add_service(descriptor, []() {
                return std::make_shared<RestartLimitProbeService>();
            }), "circuit breaker probe factory service should register")) {
            return 1;
        }

        yuan::app::Bootstrap bootstrap(app);
        if (!require(bootstrap.run(), "bootstrap should start circuit breaker probe worker")) {
            return 1;
        }

        if (!require(wait_for_supervisor_circuit_open(bootstrap),
                     "supervisor circuit should open and suppress immediate in-process restart")) {
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        bootstrap.poll_workers();
        if (!require(RestartLimitProbeService::started() == 1,
                     "supervisor circuit should defer restart while backoff is active")) {
            return 1;
        }

        if (!require(wait_for_supervisor_circuit_recovery_restart(bootstrap),
                     "supervisor circuit should allow restart after backoff")) {
            return 1;
        }

        bootstrap.shutdown();
        if (!require(RestartLimitProbeService::stopped() >= 2,
                     "circuit breaker probe service should stop failed generations")) {
            return 1;
        }
    }

    std::cout << "in-process worker runtime test passed\n";
    return 0;
}
