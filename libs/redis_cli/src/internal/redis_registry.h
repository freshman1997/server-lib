#ifndef __YUAN_REDIS_REGISTERY_H__
#define __YUAN_REDIS_REGISTERY_H__

#include "singleton/singleton.h"

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

        net::EventLoop *get_event_loop() const { return event_loop_; }

        net::Poller *get_poller() const { return poller_; }

        timer::TimerManager *get_timer_manager() const { return timer_manager_; }

        void run();

        void use_corutine();

    private:
        net::EventLoop *event_loop_;
        net::Poller *poller_;
        timer::TimerManager *timer_manager_;
    };
}

#endif // __YUAN_REDIS_REGISTERY_H__
