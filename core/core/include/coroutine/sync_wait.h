#ifndef __YUAN_COROUTINE_SYNC_WAIT_H__
#define __YUAN_COROUTINE_SYNC_WAIT_H__

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "coroutine/runtime.h"
#include "coroutine/task.h"
#include "event/event_loop.h"

namespace yuan::coroutine
{

    template <typename T>
    T sync_wait_locked(RuntimeView runtime, Task<T> task, std::string_view busy_message)
    {
        if (!runtime.event_loop()) {
            return task.resume_once_and_get_result();
        }

        struct RunLock
        {
            RuntimeView runtime;
            bool locked = false;

            ~RunLock()
            {
                if (locked) {
                    runtime.release_run_lock();
                }
            }
        } run_lock{runtime, runtime.try_acquire_run_lock()};

        if (!run_lock.locked) {
            throw std::runtime_error(std::string(busy_message));
        }

        std::optional<T> result;
        std::exception_ptr error;
        bool completed = false;

        auto driver = [&]()->Task<bool>
        {
            co_await runtime.schedule();

            try
            {
                result = co_await task;
            }
            catch (...)
            {
                error = std::current_exception();
            }

            completed = true;
            runtime.request_resume();
            co_return true;
        };

        auto driver_task = driver();
        driver_task.resume();

        while (!completed) {
            runtime.event_loop()->loop();
        }

        if (error) {
            std::rethrow_exception(error);
        }

        return std::move(*result);
    }

    template <typename T>
    T sync_wait(RuntimeView runtime, Task<T> task)
    {
        if (!runtime.event_loop()) {
            return task.resume_once_and_get_result();
        }

        std::optional<T> result;
        std::exception_ptr error;
        bool completed = false;

        auto driver = [&]()->Task<bool>
        {
            co_await runtime.schedule();

            try
            {
                result = co_await task;
            }
            catch (...)
            {
                error = std::current_exception();
            }

            completed = true;
            runtime.request_resume();
            co_return true;
        };

        auto driver_task = driver();
        driver_task.resume();

        while (!completed) {
            runtime.event_loop()->loop();
        }

        if (error) {
            std::rethrow_exception(error);
        }

        return std::move(*result);
    }

    inline void sync_wait(RuntimeView runtime, Task<void> task)
    {
        if (!runtime.event_loop()) {
            task.resume_once_and_get_result();
            return;
        }

        std::exception_ptr error;
        bool completed = false;

        auto driver = [&]()->Task<void>
        {
            co_await runtime.schedule();

            try
            {
                co_await task;
            }
            catch (...)
            {
                error = std::current_exception();
            }

            completed = true;
            runtime.request_resume();
        };

        auto driver_task = driver();
        driver_task.resume();

        while (!completed) {
            runtime.event_loop()->loop();
        }

        if (error) {
            std::rethrow_exception(error);
        }
    }

    inline void sync_wait_locked(RuntimeView runtime, Task<void> task, std::string_view busy_message)
    {
        if (!runtime.event_loop()) {
            task.resume_once_and_get_result();
            return;
        }

        struct RunLock
        {
            RuntimeView runtime;
            bool locked = false;

            ~RunLock()
            {
                if (locked) {
                    runtime.release_run_lock();
                }
            }
        } run_lock{runtime, runtime.try_acquire_run_lock()};

        if (!run_lock.locked) {
            throw std::runtime_error(std::string(busy_message));
        }

        std::exception_ptr error;
        bool completed = false;

        auto driver = [&]()->Task<void>
        {
            co_await runtime.schedule();

            try
            {
                co_await task;
            }
            catch (...)
            {
                error = std::current_exception();
            }

            completed = true;
            runtime.request_resume();
        };

        auto driver_task = driver();
        driver_task.resume();

        while (!completed) {
            runtime.event_loop()->loop();
        }

        if (error) {
            std::rethrow_exception(error);
        }
    }

} // namespace yuan::coroutine

#endif
