#include "redis_async_executor.h"

namespace yuan::redis
{
    RedisAsyncExecutor::RedisAsyncExecutor(Option option)
        : redis_(option),
          operation_timeout_ms_(option.command_timeout_ms_ != 0 ? option.command_timeout_ms_ : option.timeout_ms_),
          worker_([this] { worker_loop(); })
    {
    }

    RedisAsyncExecutor::~RedisAsyncExecutor()
    {
        close();
    }

    void RedisAsyncExecutor::close()
    {
        {
            std::lock_guard lock(mutex_);
            if (closing_) {
                return;
            }
            closing_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        redis_.close();
    }

    bool RedisAsyncExecutor::enqueue(std::function<void()> task)
    {
        {
            std::lock_guard lock(mutex_);
            if (closing_) {
                return false;
            }
            tasks_.push_back(std::move(task));
        }
        cv_.notify_one();
        return true;
    }

    void RedisAsyncExecutor::worker_loop()
    {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return closing_ || !tasks_.empty(); });
                if (closing_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }
}
