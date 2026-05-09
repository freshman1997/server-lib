#include "coroutine/completion_event.h"
#include "coroutine/runtime.h"
#include "coroutine/sync_wait.h"
#include "coroutine/task.h"
#include "event/event_loop.h"
#include "net/poller/select_poller.h"
#include "timer/wheel_timer_manager.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

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

yuan::coroutine::Task<int> immediate_value()
{
    co_return 42;
}

yuan::coroutine::Task<int> throws_value()
{
    throw std::runtime_error("task failure");
    co_return 0;
}

yuan::coroutine::Task<void> throws_void()
{
    throw std::runtime_error("void task failure");
    co_return;
}

yuan::coroutine::Task<void> detached_throws_void()
{
    throw std::runtime_error("detached task failure");
    co_return;
}

int test_task_resume_once_api()
{
    auto value_task = immediate_value();
    if (value_task.resume_once_and_get_result() != 42) {
        std::cerr << "resume_once_and_get_result should return immediate result\n";
        return 100;
    }

    bool value_thrown = false;
    try {
        auto failing = throws_value();
        (void)failing.resume_once_and_get_result();
    } catch (const std::runtime_error &) {
        value_thrown = true;
    }
    if (!value_thrown) {
        std::cerr << "Task<T> resume_once_and_get_result should rethrow exceptions\n";
        return 101;
    }

    bool void_thrown = false;
    try {
        auto failing = throws_void();
        failing.resume_once_and_get_result();
    } catch (const std::runtime_error &) {
        void_thrown = true;
    }
    if (!void_thrown) {
        std::cerr << "Task<void> resume_once_and_get_result should rethrow exceptions\n";
        return 102;
    }

    return 0;
}

int test_detached_task_exception_sink()
{
    bool sink_called = false;
    std::string message;
    yuan::coroutine::Task<void>::set_detached_exception_handler(
        [&sink_called, &message](std::exception_ptr exception) {
            sink_called = true;
            try {
                if (exception) {
                    std::rethrow_exception(exception);
                }
            } catch (const std::exception &e) {
                message = e.what();
            }
        });

    auto task = detached_throws_void();
    task.resume();
    task.detach();
    yuan::coroutine::Task<void>::clear_detached_exception_handler();

    if (!sink_called) {
        std::cerr << "detached task exception handler should be called\n";
        return 110;
    }
    if (message != "detached task failure") {
        std::cerr << "detached task exception handler should receive original exception\n";
        return 111;
    }

    return 0;
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

    const int task_api_result = test_task_resume_once_api();
    if (task_api_result != 0) {
        return task_api_result;
    }

    const int detached_sink_result = test_detached_task_exception_sink();
    if (detached_sink_result != 0) {
        return detached_sink_result;
    }

    std::cout << "coroutine runtime smoke test passed\n";
    return EXIT_SUCCESS;
}
