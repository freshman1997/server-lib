#include "coroutine/completion_event.h"
#include "coroutine/runtime.h"
#include "coroutine/sync_wait.h"
#include "coroutine/task.h"
#include "event/event_loop.h"
#include "net/poller/select_poller.h"
#include "timer/wheel_timer_manager.h"

#include <cstdlib>
#include <iostream>

namespace
{
yuan::coroutine::Task<void> notify_in_runtime(yuan::coroutine::RuntimeView runtime, yuan::coroutine::CompletionEvent &done)
{
    co_await runtime.dispatch_in_loop([&done]() {
        done.notify();
    });
}

yuan::coroutine::Task<int> run_runtime_smoke(yuan::coroutine::RuntimeView runtime)
{
    int state = 0;
    yuan::coroutine::CompletionEvent done;
    done.reset(runtime.event_loop());

    co_await runtime.schedule();

    co_await runtime.dispatch_in_loop([&state]() {
        state = 1;
    });

    if (state != 1) {
        co_return 10;
    }

    auto notifier = notify_in_runtime(runtime, done);
    notifier.resume();

    const bool notify_timed_out = co_await done.wait_for(runtime.timer_manager(), 100);
    if (notify_timed_out || state != 1) {
        co_return 20;
    }

    const bool sleep_timed_out = co_await runtime.sleep_for(10);
    if (!sleep_timed_out) {
        co_return 30;
    }

    co_return 0;
}
} // namespace

int main()
{
    yuan::timer::WheelTimerManager timer_manager;
    yuan::net::SelectPoller poller;
    yuan::net::EventLoop loop(&poller, &timer_manager);
    yuan::coroutine::RuntimeView runtime(&loop, &timer_manager);

    const int result = yuan::coroutine::sync_wait(runtime, run_runtime_smoke(runtime));
    if (result != 0) {
        std::cerr << "coroutine runtime smoke test failed: " << result << "\n";
        return result;
    }

    std::cout << "coroutine runtime smoke test passed\n";
    return EXIT_SUCCESS;
}
