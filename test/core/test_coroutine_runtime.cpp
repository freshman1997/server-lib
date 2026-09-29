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
    value_task.resume();
    const auto value = value_task.take_result();
    if (!value || *value.value != 42) {
        std::cerr << "take_result should return immediate result\n";
        return 100;
    }

    auto failing = throws_value();
    if (failing.take_result()) {
        std::cerr << "unfinished Task<T> should not report success\n";
        return 103;
    }
    failing.resume();
    if (failing.take_result()) {
        std::cerr << "Task<T> take_result should report exceptions\n";
        return 101;
    }

    auto void_failing = throws_void();
    if (void_failing.take_result()) {
        std::cerr << "unfinished Task<void> should not report success\n";
        return 104;
    }
    void_failing.resume();
    if (void_failing.take_result()) {
        std::cerr << "Task<void> take_result should report exceptions\n";
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

int test_sync_wait_error_result()
{
    auto value = yuan::coroutine::sync_wait(yuan::coroutine::RuntimeView{}, throws_value());
    auto void_result = yuan::coroutine::sync_wait(yuan::coroutine::RuntimeView{}, throws_void());
    if (!value.error || !void_result.error || value || void_result) {
        std::cerr << "sync_wait should report coroutine exceptions as results\n";
        return 120;
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

    const auto result = yuan::coroutine::sync_wait(runtime, run_runtime_smoke(runtime));
    if (!result) {
        std::cerr << "coroutine runtime smoke task failed\n";
        return 1;
    }
    if (*result.value != 0) {
        std::cerr << "coroutine runtime smoke test failed: " << *result.value << "\n";
        return *result.value;
    }

    const int task_api_result = test_task_resume_once_api();
    if (task_api_result != 0) {
        return task_api_result;
    }

    if (const int error_result = test_sync_wait_error_result(); error_result != 0) {
        return error_result;
    }

    const int detached_sink_result = test_detached_task_exception_sink();
    if (detached_sink_result != 0) {
        return detached_sink_result;
    }


    std::cout << "coroutine runtime smoke test passed\n";
    return EXIT_SUCCESS;
}
