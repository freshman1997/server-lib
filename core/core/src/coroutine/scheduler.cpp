#include "coroutine/scheduler.h"

#include "event/event_loop.h"

namespace yuan::coroutine
{

void EventLoopScheduler::post(std::coroutine_handle<> handle) noexcept
{
    if (!loop_ || !handle) {
        return;
    }

    loop_->post_coroutine(handle);
}

} // namespace yuan::coroutine
