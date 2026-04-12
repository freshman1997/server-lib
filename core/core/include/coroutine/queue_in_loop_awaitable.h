#ifndef __YUAN_COROUTINE_QUEUE_IN_LOOP_AWAITABLE_H__
#define __YUAN_COROUTINE_QUEUE_IN_LOOP_AWAITABLE_H__

#include <coroutine>
#include <functional>
#include <memory>

#include "event/event_loop.h"

namespace yuan::coroutine
{

class ScheduledQueueInLoopAwaiter
{
public:
    ScheduledQueueInLoopAwaiter(net::EventLoop *loop, std::function<void()> callback = {})
        : loop_(loop)
        , callback_(std::move(callback))
    {
    }

    bool await_ready() const noexcept
    {
        return loop_ == nullptr;
    }

    bool await_suspend(std::coroutine_handle<> handle)
    {
        if (!loop_) {
            return false;
        }

        struct State
        {
            net::EventLoop *loop = nullptr;
            std::function<void()> callback{};
            std::coroutine_handle<> handle{};
        };

        auto state = std::make_shared<State>();
        state->loop = loop_;
        state->callback = std::move(callback_);
        state->handle = handle;

        loop_->queue_in_loop([state]() {
            if (state->callback) {
                state->callback();
            }

            if (state->loop && state->handle) {
                state->loop->post_coroutine(state->handle);
            }
        });

        return true;
    }

    void await_resume() const noexcept
    {
    }

private:
    net::EventLoop *loop_ = nullptr;
    std::function<void()> callback_{};
};

inline ScheduledQueueInLoopAwaiter dispatch_in_event_loop(
    net::EventLoop *loop,
    std::function<void()> callback = {})
{
    return ScheduledQueueInLoopAwaiter(loop, std::move(callback));
}

} // namespace yuan::coroutine

#endif
