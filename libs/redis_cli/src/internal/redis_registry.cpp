#include "redis_registry.h"
#include "net/poller/select_poller.h"
#include "event/event_loop.h"
#include "timer/wheel_timer_manager.h"
#include "redis_cli_manager.h"

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
        RedisCliManager::get_instance()->release_all();

        delete event_loop_;
        delete poller_;
        delete timer_manager_;
    }

    void RedisRegistry::run()
    {
        event_loop_->loop();
    }

    void RedisRegistry::use_corutine()
    {
        event_loop_->set_use_coroutine(true);
    }
}