#ifndef __YUAN_REDIS_REGISTERY_H__
#define __YUAN_REDIS_REGISTERY_H__

#include "coroutine/runtime.h"
#include "singleton/singleton.h"

#include <memory>
#include <mutex>
#include <thread>

namespace yuan::net
{
    class Poller;
    class EventLoop;
    class NetworkRuntime;
}

namespace yuan::timer
{
    class TimerManager;
}

namespace yuan::redis
{
    class RedisRegistry : public singleton::Singleton<RedisRegistry>
    {
    public:
        RedisRegistry();

        ~RedisRegistry();

        RedisRegistry(const RedisRegistry &) = delete;
        RedisRegistry &operator=(const RedisRegistry &) = delete;

        net::EventLoop *get_event_loop() const;

        net::Poller *get_poller() const;

        timer::TimerManager *get_timer_manager() const;

        yuan::coroutine::RuntimeView get_coroutine_runtime() const;

        bool is_event_loop_thread() const;

        bool try_acquire_run_lock();

        void release_run_lock();

        void drain_pending();

        void run();

    private:
        std::unique_ptr<net::NetworkRuntime> runtime_;
        mutable std::recursive_mutex run_mutex_;
        mutable std::thread::id owner_thread_;
    };
}

#endif // __YUAN_REDIS_REGISTERY_H__
