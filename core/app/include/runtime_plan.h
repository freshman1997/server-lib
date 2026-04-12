#ifndef __YUAN_APP_RUNTIME_PLAN_H__
#define __YUAN_APP_RUNTIME_PLAN_H__

#include "runtime_context.h"

#include <string>

namespace yuan::app
{

enum class EventLoopMode
{
    reactor_per_service,
    parallel_service_reactors,
    process_reactors,
};

struct RuntimePlan
{
    RunMode run_mode = RunMode::single_thread;
    EventLoopMode event_loop_mode = EventLoopMode::reactor_per_service;
    bool coroutine_compatible = true;
    bool implemented = true;
    bool parallel_service_start = false;
    std::string note;
};

RuntimePlan derive_runtime_plan(const RuntimeContext &context);
const char *to_string(EventLoopMode mode) noexcept;

} // namespace yuan::app

#endif
