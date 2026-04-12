#ifndef __YUAN_COROUTINE_TASK_H__
#define __YUAN_COROUTINE_TASK_H__

#include <coroutine>
#include <exception>
#include <utility>

namespace yuan::coroutine
{

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
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }
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

        final_awaiter final_suspend() noexcept { return {}; }

        void unhandled_exception() { exception_ = std::current_exception(); }

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

    T get_result() const
    {
        if (handle_ && handle_.promise().exception_) {
            std::rethrow_exception(handle_.promise().exception_);
        }
        return handle_.promise().value_;
    }

    T execute()
    {
        resume();
        return get_result();
    }

    operator T() const
    {
        return get_result();
    }

    class awaiter
    {
    public:
        explicit awaiter(std::coroutine_handle<promise_type> handle) noexcept
            : handle_(handle)
        {
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
            return handle_.promise().value_;
        }

    private:
        std::coroutine_handle<promise_type> handle_{};
    };

    awaiter operator co_await() noexcept
    {
        return awaiter(handle_);
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

        Task get_return_object()
        {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }
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

        final_awaiter final_suspend() noexcept { return {}; }

        void unhandled_exception() { exception_ = std::current_exception(); }

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

    void get_result() const
    {
        if (handle_ && handle_.promise().exception_) {
            std::rethrow_exception(handle_.promise().exception_);
        }
    }

    void execute()
    {
        resume();
        get_result();
    }

    class awaiter
    {
    public:
        explicit awaiter(std::coroutine_handle<promise_type> handle) noexcept
            : handle_(handle)
        {
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
        std::coroutine_handle<promise_type> handle_{};
    };

    awaiter operator co_await() noexcept
    {
        return awaiter(handle_);
    }

private:
    std::coroutine_handle<promise_type> handle_{};
};

} // namespace yuan::coroutine

#endif
