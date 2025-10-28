#include "redis_registry.h"
#include "net/poller/select_poller.h"
#include "event/event_loop.h"
#include "timer/wheel_timer_manager.h"

namespace yuan::redis
{
    RedisRegistry::RedisRegistry()
    {
        poller_ = new net::SelectPoller();
        timer_manager_ = new timer::WheelTimerManager();
        event_loop_ = new net::EventLoop(poller_, timer_manager_);
    }

    RedisRegistry::~RedisRegistry()
    {
        delete event_loop_;
        delete poller_;
        delete timer_manager_;
    }
}