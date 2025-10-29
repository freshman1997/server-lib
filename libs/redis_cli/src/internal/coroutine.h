#ifndef __YUAN_REDIS_COROUTINE_H__
#define __YUAN_REDIS_COROUTINE_H__

#include <coroutine>
#include <memory>
#include "redis_value.h"

namespace yuan::redis
{
    class RedisClient;

    struct SimpleTask 
    {
        struct promise_type 
        {
            std::shared_ptr<RedisValue> value_;

            SimpleTask get_return_object() { return SimpleTask { std::coroutine_handle<promise_type>::from_promise(*this) }; }
            std::suspend_never initial_suspend() { return {}; } // 协程立即执行
            std::suspend_always final_suspend() noexcept { return {}; }
            void unhandled_exception() {}
            std::suspend_always yield_value(std::shared_ptr<RedisValue> value) 
            {
                value_ = value;
                return {};
            }

            void return_value(std::shared_ptr<RedisValue> value) {
                value_ = std::move(value);
            }
        };

        std::coroutine_handle<promise_type> handle_;

        SimpleTask(std::coroutine_handle<promise_type> h) : handle_(h) {}
        ~SimpleTask() { if (handle_) handle_.destroy(); }
        
        // 获取最终结果
        std::shared_ptr<RedisValue> get_result() {
            return handle_.promise().value_;
        }
        
        bool done() const { return handle_.done(); }
        
        operator std::shared_ptr<RedisValue>() const {
            return handle_.promise().value_;
        }

        // 执行协程
        std::shared_ptr<RedisValue> execute() {
            if (handle_ && !handle_.done()) {
                handle_.resume();
            }
            return get_result();
        }
    };
}

#endif // __YUAN_REDIS_COROUTINE_H__