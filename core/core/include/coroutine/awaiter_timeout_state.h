#ifndef __YUAN_COROUTINE_AWAITER_TIMEOUT_STATE_H__
#define __YUAN_COROUTINE_AWAITER_TIMEOUT_STATE_H__

#include <coroutine>
#include <cstdint>
#include <memory>

#include "coroutine/runtime_view.h"
#include "event/event_loop.h"
#include "timer/timer.h"
#include "timer/timer_handle.h"
#include "timer/timer_util.hpp"

namespace yuan::coroutine::detail
{
    struct AwaiterTimeoutState
    {
        net::EventLoop *loop = nullptr;
        timer::TimerHandle timer;
        std::coroutine_handle<> handle{};
        bool timed_out = false;
        bool completed = false;
        bool cancelled = false;
    };

    inline timer::TimerHandle arm_awaiter_timeout(RuntimeView runtime,
                                                  uint32_t timeout_ms,
                                                  const std::shared_ptr<AwaiterTimeoutState> &state) noexcept
    {
        if (!state || timeout_ms == 0 || !runtime.timer_manager()) {
            return {};
        }

        state->timer = timer::TimerUtil::build_timeout_handle(
            runtime.timer_manager(),
            timeout_ms,
            [weak_state = std::weak_ptr<AwaiterTimeoutState>(state)]() {
                auto state = weak_state.lock();
                if (!state || state->cancelled || state->completed) {
                    return;
                }
                state->timed_out = true;
                state->timer.reset();
                if (state->loop && state->handle) {
                    state->loop->post_coroutine(state->handle);
                }
            });
        return state->timer;
    }

    inline void cancel_awaiter_timeout(const std::shared_ptr<AwaiterTimeoutState> &state) noexcept
    {
        if (!state) {
            return;
        }
        state->cancelled = true;
        state->timer.cancel();
        state->timer.reset();
    }
}

#endif
