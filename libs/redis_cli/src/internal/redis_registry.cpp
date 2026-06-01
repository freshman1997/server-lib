#include "redis_registry.h"
#include "event/event_loop.h"
#include "net/runtime/network_runtime.h"
#include "net/poller/poller.h"
#include "timer/timer_manager.h"

#include <thread>

namespace yuan::redis
{
    RedisRegistry::RedisRegistry()
    {
        runtime_ = std::make_unique<net::NetworkRuntime>();
    }

    RedisRegistry::~RedisRegistry()
    {
        drain_pending();
        runtime_.reset();
    }

    net::EventLoop *RedisRegistry::get_event_loop() const
    {
        return runtime_ ? runtime_->event_loop() : nullptr;
    }

    net::Poller *RedisRegistry::get_poller() const
    {
        return runtime_ ? runtime_->poller() : nullptr;
    }

    timer::TimerManager *RedisRegistry::get_timer_manager() const
    {
        return runtime_ ? runtime_->timer_manager() : nullptr;
    }

    yuan::coroutine::RuntimeView RedisRegistry::get_coroutine_runtime() const
    {
        return {get_event_loop(), get_timer_manager(), &run_mutex_, &owner_thread_};
    }

    void RedisRegistry::run()
    {
        const auto runtime = get_coroutine_runtime();
        if (!runtime.try_acquire_run_lock()) {
            return;
        }
        if (runtime_) {
            runtime_->run();
        }
        runtime.release_run_lock();
    }

    void RedisRegistry::drain_pending()
    {
        const auto runtime = get_coroutine_runtime();
        if (!runtime.try_acquire_run_lock()) {
            return;
        }
        auto *loop = get_event_loop();
        if (loop) {
            loop->queue_in_loop([loop]() {
                loop->request_coroutine_resume();
            });
        }
        if (runtime_) {
            runtime_->run();
        }
        runtime.release_run_lock();
    }

}
