#ifndef __YUAN_COROUTINE_SYNC_WAIT_H__
#define __YUAN_COROUTINE_SYNC_WAIT_H__

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>

#include "coroutine/runtime.h"
#include "coroutine/task.h"
#include "event/event_loop.h"

namespace yuan::coroutine
{
    template <typename T>
    TaskResult<T> sync_wait(RuntimeView runtime, Task<T> task)
    {
        if (!runtime.event_loop()) {
            task.resume();
            return task.take_result();
        }

        TaskResult<T> result;
        bool completed = false;
        auto driver = [&]() -> Task<void> {
            co_await runtime.schedule();
            try {
                result.value = co_await task;
            } catch (...) {
                result.error = std::current_exception();
            }
            completed = true;
            runtime.request_resume();
        };

        auto driver_task = driver();
        driver_task.resume();
        while (!completed) {
            runtime.event_loop()->loop();
        }
        result.completed = true;
        return result;
    }

    inline TaskVoidResult sync_wait(RuntimeView runtime, Task<void> task)
    {
        if (!runtime.event_loop()) {
            task.resume();
            return task.take_result();
        }

        TaskVoidResult result;
        bool completed = false;
        auto driver = [&]() -> Task<void> {
            co_await runtime.schedule();
            try {
                co_await task;
            } catch (...) {
                result.error = std::current_exception();
            }
            result.completed = true;
            completed = true;
            runtime.request_resume();
        };

        auto driver_task = driver();
        driver_task.resume();
        while (!completed) {
            runtime.event_loop()->loop();
        }
        return result;
    }

    template <typename T>
    TaskResult<T> sync_wait_locked(RuntimeView runtime,
                                   Task<T> task,
                                   std::string_view busy_message)
    {
        if (!runtime.event_loop()) {
            task.resume();
            return task.take_result();
        }

        struct RunLock {
            RuntimeView runtime;
            bool locked;
            ~RunLock() { if (locked) runtime.release_run_lock(); }
        } run_lock{runtime, runtime.try_acquire_run_lock()};

        if (!run_lock.locked) {
            return TaskResult<T>{false, std::nullopt,
                                 std::make_exception_ptr(std::runtime_error(std::string(busy_message)))};
        }
        return sync_wait(runtime, std::move(task));
    }

    inline TaskVoidResult sync_wait_locked(RuntimeView runtime,
                                           Task<void> task,
                                           std::string_view busy_message)
    {
        if (!runtime.event_loop()) {
            task.resume();
            return task.take_result();
        }

        struct RunLock {
            RuntimeView runtime;
            bool locked;
            ~RunLock() { if (locked) runtime.release_run_lock(); }
        } run_lock{runtime, runtime.try_acquire_run_lock()};

        if (!run_lock.locked) {
            return TaskVoidResult{false,
                std::make_exception_ptr(std::runtime_error(std::string(busy_message)))};
        }
        return sync_wait(runtime, std::move(task));
    }
}

#endif
