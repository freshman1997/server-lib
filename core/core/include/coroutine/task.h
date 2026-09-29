#ifndef __YUAN_COROUTINE_TASK_H__
#define __YUAN_COROUTINE_TASK_H__

#include <coroutine>
#include <cstdio>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace yuan::coroutine
{
    inline std::string task_error_message(const std::exception_ptr &error)
    {
        if (!error) {
            return {};
        }
        try {
            std::rethrow_exception(error);
        } catch (const std::exception &e) {
            return e.what();
        } catch (...) {
            return "unknown coroutine error";
        }
    }

    template <typename T>
    struct TaskResult
    {
        bool completed = false;
        std::optional<T> value;
        std::exception_ptr error;

        explicit operator bool() const noexcept { return completed && !error && value.has_value(); }
    };

    struct TaskVoidResult
    {
        bool completed = false;
        std::exception_ptr error;

        explicit operator bool() const noexcept { return completed && !error; }
    };

    template <typename T>
    class Task
    {
    public:
        struct promise_type
        {
            T value_{};
            std::exception_ptr exception_;
            std::coroutine_handle<> continuation_{};

            Task get_return_object()
            {
                return Task{ std::coroutine_handle<promise_type>::from_promise(*this) };
            }

            std::suspend_always initial_suspend() noexcept
            {
                return {};
            }
            struct final_awaiter
            {
                bool await_ready() const noexcept
                {
                    return false;
                }

                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> handle) const noexcept
                {
                    const auto continuation = handle.promise().continuation_;
                    return continuation ? continuation : std::noop_coroutine();
                }

                void await_resume() const noexcept
                {
                }
            };

            final_awaiter final_suspend() noexcept
            {
                return {};
            }

            void unhandled_exception()
            {
                exception_ = std::current_exception();
            }

            std::suspend_always yield_value(T value)
            {
                value_ = std::move(value);
                return {};
            }

            void return_value(T value)
            {
                value_ = std::move(value);
            }
        };

        Task() = default;

        explicit Task(std::coroutine_handle<promise_type> handle)
            : handle_(handle)
        {
        }

        Task(const Task &) = delete;
        Task &operator=(const Task &) = delete;

        Task(Task &&other) noexcept
            : handle_(std::exchange(other.handle_, {}))
        {
        }

        Task &operator=(Task &&other) noexcept
        {
            if (this != &other) {
                if (handle_) {
                    handle_.destroy();
                }
                handle_ = std::exchange(other.handle_, {});
            }
            return *this;
        }

        ~Task()
        {
            if (handle_) {
                handle_.destroy();
            }
        }

        bool done() const
        {
            return !handle_ || handle_.done();
        }

        void resume()
        {
            if (handle_ && !handle_.done()) {
                handle_.resume();
            }
        }

        TaskResult<T> take_result() const
        {
            TaskResult<T> result;
            if (!handle_ || !handle_.done()) {
                return result;
            }
            result.completed = true;
            result.error = handle_.promise().exception_;
            if (!result.error) {
                result.value = std::move(handle_.promise().value_);
            }
            return result;
        }

        class awaiter
        {
        public:
            explicit awaiter(std::coroutine_handle<promise_type> handle) noexcept
                : handle_(handle)
            {
            }

            awaiter(const awaiter &) = delete;
            awaiter &operator=(const awaiter &) = delete;

            awaiter(awaiter &&other) noexcept
                : handle_(std::exchange(other.handle_, {}))
            {
            }

            awaiter &operator=(awaiter &&other) noexcept
            {
                if (this != &other) {
                    destroy_if_needed();
                    handle_ = std::exchange(other.handle_, {});
                }
                return *this;
            }

            ~awaiter()
            {
                destroy_if_needed();
            }

            bool await_ready() const noexcept
            {
                return !handle_ || handle_.done();
            }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept
            {
                handle_.promise().continuation_ = continuation;
                return handle_;
            }

            T await_resume() const
            {
                if (handle_ && handle_.promise().exception_) {
                    std::rethrow_exception(handle_.promise().exception_);
                }
                return std::move(handle_.promise().value_);
            }

        private:
            void destroy_if_needed() noexcept
            {
                if (handle_) {
                    handle_.destroy();
                    handle_ = {};
                }
            }

            std::coroutine_handle<promise_type> handle_{};
        };

        awaiter operator co_await() noexcept
        {
            return awaiter(std::exchange(handle_, {}));
        }

    private:
        std::coroutine_handle<promise_type> handle_{};
    };

    template <>
    class Task<void>
    {
    public:
        struct promise_type
        {
            std::exception_ptr exception_;
            std::coroutine_handle<> continuation_{};
            bool detached_ = false;

            static std::function<void(std::exception_ptr)> &detached_exception_handler()
            {
                static std::function<void(std::exception_ptr)> handler;
                return handler;
            }

            static std::mutex &detached_exception_mutex()
            {
                static std::mutex mutex;
                return mutex;
            }

            static void notify_detached_exception(const std::exception_ptr &exception) noexcept
            {
                if (!exception) {
                    return;
                }

                try {
                    std::function<void(std::exception_ptr)> handler;
                    {
                        std::lock_guard<std::mutex> lock(detached_exception_mutex());
                        handler = detached_exception_handler();
                    }
                    if (handler) {
                        handler(exception);
                    } else {
                        std::fprintf(stderr, "Detached task failed: %s\n", task_error_message(exception).c_str());
                    }
                } catch (...) {
                    std::fputs("Detached task exception handler failed\n", stderr);
                }
            }

            Task get_return_object()
            {
                return Task{ std::coroutine_handle<promise_type>::from_promise(*this) };
            }

            std::suspend_always initial_suspend() noexcept
            {
                return {};
            }
            struct final_awaiter
            {
                bool await_ready() const noexcept
                {
                    return false;
                }

                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> handle) const noexcept
                {
                    if (handle.promise().detached_) {
                        promise_type::notify_detached_exception(handle.promise().exception_);
                        handle.destroy();
                        return std::noop_coroutine();
                    }
                    const auto continuation = handle.promise().continuation_;
                    return continuation ? continuation : std::noop_coroutine();
                }

                void await_resume() const noexcept
                {
                }
            };

            final_awaiter final_suspend() noexcept
            {
                return {};
            }

            void unhandled_exception()
            {
                exception_ = std::current_exception();
            }

            void return_void() noexcept
            {
            }
        };

        Task() = default;

        explicit Task(std::coroutine_handle<promise_type> handle)
            : handle_(handle)
        {
        }

        Task(const Task &) = delete;
        Task &operator=(const Task &) = delete;

        Task(Task &&other) noexcept
            : handle_(std::exchange(other.handle_, {}))
        {
        }

        Task &operator=(Task &&other) noexcept
        {
            if (this != &other) {
                if (handle_) {
                    handle_.destroy();
                }
                handle_ = std::exchange(other.handle_, {});
            }
            return *this;
        }

        ~Task()
        {
            if (handle_) {
                handle_.destroy();
            }
        }

        static void set_detached_exception_handler(std::function<void(std::exception_ptr)> handler)
        {
            std::lock_guard<std::mutex> lock(promise_type::detached_exception_mutex());
            promise_type::detached_exception_handler() = std::move(handler);
        }

        static void clear_detached_exception_handler()
        {
            std::lock_guard<std::mutex> lock(promise_type::detached_exception_mutex());
            promise_type::detached_exception_handler() = {};
        }

        void detach() noexcept
        {
            if (handle_) {
                handle_.promise().detached_ = true;
                if (handle_.done()) {
                    promise_type::notify_detached_exception(handle_.promise().exception_);
                    handle_.destroy();
                }
                handle_ = nullptr;
            }
        }

        bool done() const
        {
            return !handle_ || handle_.done();
        }

        void resume()
        {
            if (handle_ && !handle_.done()) {
                handle_.resume();
            }
        }

        TaskVoidResult take_result() const
        {
            TaskVoidResult result;
            if (handle_ && handle_.done()) {
                result.completed = true;
                result.error = handle_.promise().exception_;
            }
            return result;
        }

        class awaiter
        {
        public:
            explicit awaiter(std::coroutine_handle<promise_type> handle) noexcept
                : handle_(handle)
            {
            }

            awaiter(const awaiter &) = delete;
            awaiter &operator=(const awaiter &) = delete;

            awaiter(awaiter &&other) noexcept
                : handle_(std::exchange(other.handle_, {}))
            {
            }

            awaiter &operator=(awaiter &&other) noexcept
            {
                if (this != &other) {
                    destroy_if_needed();
                    handle_ = std::exchange(other.handle_, {});
                }
                return *this;
            }

            ~awaiter()
            {
                destroy_if_needed();
            }

            bool await_ready() const noexcept
            {
                return !handle_ || handle_.done();
            }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept
            {
                handle_.promise().continuation_ = continuation;
                return handle_;
            }

            void await_resume() const
            {
                if (handle_ && handle_.promise().exception_) {
                    std::rethrow_exception(handle_.promise().exception_);
                }
            }

        private:
            void destroy_if_needed() noexcept
            {
                if (handle_) {
                    handle_.destroy();
                    handle_ = {};
                }
            }

            std::coroutine_handle<promise_type> handle_{};
        };

        awaiter operator co_await() noexcept
        {
            return awaiter(std::exchange(handle_, {}));
        }

    private:
        std::coroutine_handle<promise_type> handle_{};
    };

} // namespace yuan::coroutine

#endif
