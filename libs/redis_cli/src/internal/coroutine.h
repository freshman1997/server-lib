#ifndef __YUAN_REDIS_COROUTINE_H__
#define __YUAN_REDIS_COROUTINE_H__

#include <coroutine>
#include <utility>

namespace yuan::redis
{
    class RedisClient;

    template <typename T>
    struct SimpleTask 
    {
        struct promise_type 
        {
            T value_;

            SimpleTask get_return_object() { return SimpleTask { std::coroutine_handle<promise_type>::from_promise(*this) }; }
            std::suspend_never initial_suspend() { return {}; } // 协程立即执行
            std::suspend_always final_suspend() noexcept { return {}; }
            void unhandled_exception() {}
            std::suspend_always yield_value(T && value) 
            {
                value_ = std::move(value);
                return {};
            }

            void return_value(T && value) {
                value_ = std::move(value);
            }
        };

        std::coroutine_handle<promise_type> handle_;

        SimpleTask(std::coroutine_handle<promise_type> h) : handle_(h) {}
        ~SimpleTask() { if (handle_) handle_.destroy(); }
        
        // 获取最终结果
        T get_result() {
            return handle_.promise().value_;
        }
        
        bool done() const { return handle_.done(); }
        
        operator T() const {
            return handle_.promise().value_;
        }

        // 执行协程
        T execute() {
            if (handle_ && !handle_.done()) {
                handle_.resume();
            }
            return get_result();
        }
    };
}

#endif // __YUAN_REDIS_COROUTINE_H__