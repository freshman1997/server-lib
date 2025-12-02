#ifndef __COROUTINE_HELPER_H__
#define __COROUTINE_HELPER_H__
#include <coroutine>
#include <utility>

namespace yuan::rpc 
{
    template <typename T>
    class CoroutineHelper
    {
    public:
        struct promise_type 
        {
            T value_;

            CoroutineHelper get_return_object() { return CoroutineHelper { std::coroutine_handle<promise_type>::from_promise(*this) }; }
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

        CoroutineHelper(std::coroutine_handle<promise_type> handle) : handle_(handle) {}
        ~CoroutineHelper() { handle_.destroy(); }

        // 获取最终结果
        T get_result() { return handle_.promise().value_; }

        bool done() const { return handle_.done(); }

        void resume() { handle_.resume(); }

        T execute() 
        {
            if (!handle_.done()) {
                handle_.resume();
            }
            return get_result();
        }
        
    private:
        std::coroutine_handle<promise_type> handle_;
    };
}

#endif // __COROUTINE_HELPER_H__
