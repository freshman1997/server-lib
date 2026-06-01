#ifndef __YUAN_REDIS_REGISTERY_H__
#define __YUAN_REDIS_REGISTERY_H__

#include "coroutine/runtime.h"
#include "singleton/singleton.h"

#include <mutex>
#include <thread>

namespace yuan::net
{
    class Poller;
    class EventLoop;
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

        net::EventLoop *get_event_loop() const { return event_loop_; }

        net::Poller *get_poller() const { return poller_; }

        timer::TimerManager *get_timer_manager() const { return timer_manager_; }

        yuan::coroutine::RuntimeView get_coroutine_runtime() const;

        bool is_event_loop_thread() const;

        bool try_acquire_run_lock();

        void release_run_lock();

        void drain_pending();

        void run();

    private:
        net::EventLoop *event_loop_;
        net::Poller *poller_;
        timer::TimerManager *timer_manager_;
        mutable std::recursive_mutex run_mutex_;
        mutable std::thread::id owner_thread_;
    };
}

#endif // __YUAN_REDIS_REGISTERY_H__