#include "redis_registry.h"
#include "net/poller/select_poller.h"
#include "event/event_loop.h"
#include "timer/wheel_timer_manager.h"

#include <thread>

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
        drain_pending();
        delete event_loop_;
        delete poller_;
        delete timer_manager_;
    }

    yuan::coroutine::RuntimeView RedisRegistry::get_coroutine_runtime() const
    {
        return {event_loop_, timer_manager_, &run_mutex_, &owner_thread_};
    }

    void RedisRegistry::run()
    {
        const auto runtime = get_coroutine_runtime();
        if (!runtime.try_acquire_run_lock()) {
            return;
        }
        event_loop_->loop();
        runtime.release_run_lock();
    }

    void RedisRegistry::drain_pending()
    {
        const auto runtime = get_coroutine_runtime();
        if (!runtime.try_acquire_run_lock()) {
            return;
        }
        event_loop_->queue_in_loop([this]() {
            event_loop_->request_coroutine_resume();
        });
        event_loop_->loop();
        runtime.release_run_lock();
    }

}
