#include "runtime_plan.h"

namespace yuan::app
{

RuntimePlan derive_runtime_plan(const RuntimeContext &context)
{
    RuntimePlan plan;
    plan.run_mode = context.run_mode;

    switch (context.run_mode) {
    case RunMode::single_thread:
        plan.event_loop_mode = EventLoopMode::reactor_per_service;
        plan.coroutine_compatible = true;
        plan.implemented = true;
        plan.parallel_service_start = false;
        plan.note = "reactor stays service-owned; application starts services sequentially";
        break;
    case RunMode::multi_thread:
        plan.event_loop_mode = EventLoopMode::parallel_service_reactors;
        plan.coroutine_compatible = true;
        plan.implemented = true;
        plan.parallel_service_start = true;
        plan.note = "reactor remains service-owned; services may start in parallel threads";
        break;
    case RunMode::multi_process:
        plan.event_loop_mode = EventLoopMode::process_reactors;
        plan.coroutine_compatible = true;
#ifdef _WIN32
        plan.implemented = false;
        plan.note = "multi-process supervisor is not implemented on Windows/MinGW yet";
#else
        plan.implemented = true;
        plan.note = "POSIX uses a minimal service-per-process supervisor; each worker keeps a local reactor runtime";
#endif
        plan.parallel_service_start = false;
        break;
    default:
        plan.event_loop_mode = EventLoopMode::reactor_per_service;
        plan.coroutine_compatible = false;
        plan.implemented = false;
        plan.parallel_service_start = false;
        plan.note = "unknown run mode";
        break;
    }

    return plan;
}

const char *to_string(EventLoopMode mode) noexcept
{
    switch (mode) {
    case EventLoopMode::reactor_per_service:
        return "reactor_per_service";
    case EventLoopMode::parallel_service_reactors:
        return "parallel_service_reactors";
    case EventLoopMode::process_reactors:
        return "process_reactors";
    default:
        return "unknown";
    }
}

} // namespace yuan::app
