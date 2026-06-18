#ifndef YUAN_REDIS_ASYNC_EXECUTOR_H
#define YUAN_REDIS_ASYNC_EXECUTOR_H

#include "coroutine/runtime_view.h"
#include "event/event_loop.h"
#include "option.h"
#include "redis_client.h"

#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <atomic>
#include <thread>
#include <type_traits>
#include <stdexcept>
#include <variant>

namespace yuan::redis
{
    class RedisAsyncExecutor
    {
    public:
        explicit RedisAsyncExecutor(Option option);
        ~RedisAsyncExecutor();

        RedisAsyncExecutor(const RedisAsyncExecutor &) = delete;
        RedisAsyncExecutor &operator=(const RedisAsyncExecutor &) = delete;

        void close();

        template <typename Fn>
        auto submit(yuan::coroutine::RuntimeView resume_runtime, Fn &&fn)
        {
            using Result = std::invoke_result_t<Fn, RedisClient &>;
            using ResultStorage = std::conditional_t<std::is_void_v<Result>, std::monostate, Result>;

            struct State
            {
                std::mutex mutex;
                std::optional<ResultStorage> result;
                std::exception_ptr exception;
                std::atomic_bool completed{false};
            };

            struct Awaitable
            {
                RedisAsyncExecutor *executor = nullptr;
                yuan::coroutine::RuntimeView resume_runtime;
                std::decay_t<Fn> fn;
                std::shared_ptr<State> state = std::make_shared<State>();

                bool await_ready() const noexcept { return false; }

                void await_suspend(std::coroutine_handle<> handle)
                {
                    auto state_copy = state;
                    auto resume_runtime_copy = resume_runtime;
                    auto resume_once = [state_copy, resume_runtime_copy, handle] {
                        if (!state_copy->completed.exchange(true)) {
                            if (auto *loop = resume_runtime_copy.event_loop()) {
                                loop->queue_in_loop([handle] {
                                    handle.resume();
                                });
                            } else {
                                handle.resume();
                            }
                        }
                    };
                    if (!executor->enqueue([state_copy, resume_once, fn = std::move(fn), executor = executor]() mutable {
                        try {
                            if constexpr (std::is_void_v<Result>) {
                                fn(executor->redis_);
                                std::lock_guard lock(state_copy->mutex);
                                if (state_copy->completed.load()) {
                                    return;
                                }
                                state_copy->result.emplace();
                            } else {
                                auto result = fn(executor->redis_);
                                std::lock_guard lock(state_copy->mutex);
                                if (state_copy->completed.load()) {
                                    return;
                                }
                                state_copy->result = std::move(result);
                            }
                        } catch (...) {
                            std::lock_guard lock(state_copy->mutex);
                            if (state_copy->completed.load()) {
                                return;
                            }
                            state_copy->exception = std::current_exception();
                        }
                        resume_once();
                    })) {
                        {
                            std::lock_guard lock(state_copy->mutex);
                            state_copy->exception = std::make_exception_ptr(std::runtime_error("redis async executor is closed"));
                        }
                        resume_once();
                    } else if (executor->operation_timeout_ms_ > 0) {
                        auto state_timeout = state_copy;
                        resume_runtime_copy.schedule(static_cast<std::uint32_t>(executor->operation_timeout_ms_), [state_timeout, resume_once] {
                            {
                                std::lock_guard lock(state_timeout->mutex);
                                state_timeout->exception = std::make_exception_ptr(std::runtime_error("redis async executor timeout"));
                            }
                            resume_once();
                        });
                    }
                }

                decltype(auto) await_resume()
                {
                    std::lock_guard lock(state->mutex);
                    if (state->exception) {
                        std::rethrow_exception(state->exception);
                    }
                    if constexpr (std::is_void_v<Result>) {
                        return;
                    } else {
                        return std::move(*state->result);
                    }
                }
            };

            return Awaitable{this, resume_runtime, std::forward<Fn>(fn)};
        }

        template <typename Fn, typename Callback>
        void submit_callback(yuan::coroutine::RuntimeView resume_runtime, Fn &&fn, Callback &&callback, std::uint32_t timeout_ms = 0)
        {
            using Result = std::invoke_result_t<Fn, RedisClient &>;
            using ResultStorage = std::conditional_t<std::is_void_v<Result>, std::monostate, Result>;
            using ResultOptional = std::optional<ResultStorage>;

            struct State
            {
                std::mutex mutex;
                ResultOptional result;
                std::exception_ptr exception;
                std::atomic_bool completed{false};
            };

            auto state = std::make_shared<State>();
            auto callback_ptr = std::make_shared<std::decay_t<Callback>>(std::forward<Callback>(callback));
            auto post_callback = [resume_runtime, state, callback_ptr]() mutable {
                auto invoke = [state, callback_ptr]() mutable {
                    ResultOptional result;
                    std::exception_ptr exception;
                    {
                        std::lock_guard lock(state->mutex);
                        result = std::move(state->result);
                        exception = state->exception;
                    }
                    (*callback_ptr)(std::move(result), exception);
                };
                if (auto *loop = resume_runtime.event_loop()) {
                    loop->queue_in_loop(std::move(invoke));
                } else {
                    invoke();
                }
            };

            auto task = [this,
                         fn = std::decay_t<Fn>(std::forward<Fn>(fn)),
                         state,
                         post_callback = std::move(post_callback)]() mutable {
                try {
                    if constexpr (std::is_void_v<Result>) {
                        fn(redis_);
                        std::lock_guard lock(state->mutex);
                        if (state->completed.load()) {
                            return;
                        }
                        state->result.emplace();
                    } else {
                        auto result = fn(redis_);
                        std::lock_guard lock(state->mutex);
                        if (state->completed.load()) {
                            return;
                        }
                        state->result = std::move(result);
                    }
                } catch (...) {
                    std::lock_guard lock(state->mutex);
                    if (state->completed.load()) {
                        return;
                    }
                    state->exception = std::current_exception();
                }
                if (!state->completed.exchange(true)) {
                    post_callback();
                }
            };

            if (!enqueue(std::move(task))) {
                {
                    std::lock_guard lock(state->mutex);
                    state->exception = std::make_exception_ptr(std::runtime_error("redis async executor is closed"));
                }
                if (!state->completed.exchange(true)) {
                    post_callback();
                }
                return;
            }

            const auto effective_timeout_ms = timeout_ms != 0 ? timeout_ms : operation_timeout_ms_;
            if (effective_timeout_ms > 0) {
                resume_runtime.schedule(effective_timeout_ms, [state, post_callback]() mutable {
                    {
                        std::lock_guard lock(state->mutex);
                        state->exception = std::make_exception_ptr(std::runtime_error("redis async executor timeout"));
                    }
                    if (!state->completed.exchange(true)) {
                        post_callback();
                    }
                });
            }
        }

    private:
        [[nodiscard]] bool enqueue(std::function<void()> task);
        void worker_loop();

        RedisClient redis_;
        std::uint32_t operation_timeout_ms_ = 0;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<std::function<void()>> tasks_;
        std::thread worker_;
        bool closing_ = false;
    };
}

#endif
