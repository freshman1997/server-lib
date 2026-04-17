#include "net/runtime/network_runtime.h"
#include "event/event_loop.h"
#include "net/acceptor/acceptor.h"
#include "net/channel/channel.h"
#include "net/connection/connection.h"
#include "net/connector/connector.h"
#include "net/handler/connection_handler.h"
#include "net/handler/connector_handler.h"
#include "net/poller/epoll_poller.h"
#include "net/poller/kqueue_poller.h"
#include "net/poller/select_poller.h"
#include "timer/wheel_timer_manager.h"
#include "timer/timer_util.hpp"

namespace yuan::net
{

    NetworkRuntime::NetworkRuntime()
        : owns_(true)
    {
        owned_poller_.reset(create_default_poller());
        owned_poller_->init();
        owned_timer_manager_.reset(new timer::WheelTimerManager());
        owned_loop_.reset(new EventLoop(owned_poller_.get(), owned_timer_manager_.get()));

        poller_ = owned_poller_.get();
        timer_manager_ = owned_timer_manager_.get();
        loop_ = owned_loop_.get();
    }

    NetworkRuntime::NetworkRuntime(EventLoop * loop, timer::TimerManager * tm)
        : loop_(loop), timer_manager_(tm), poller_(nullptr), owns_(false)
    {
    }

    NetworkRuntime::~NetworkRuntime()
    {
        stop();

        owned_loop_.reset();
        owned_timer_manager_.reset();
        owned_poller_.reset();

        if (!owns_) {
            loop_ = nullptr;
            timer_manager_ = nullptr;
            poller_ = nullptr;
        }
    }

    Poller *NetworkRuntime::create_default_poller()
    {
#if defined(__unix__)
        return new EpollPoller;
#elif defined(__APPLE__)
        return new KQueuePoller;
#else
        return new SelectPoller;
#endif
    }

    EventLoop *NetworkRuntime::event_loop() const noexcept
    {
        return loop_;
    }

    timer::TimerManager *NetworkRuntime::timer_manager() const noexcept
    {
        return timer_manager_;
    }

    Poller *NetworkRuntime::poller() const noexcept
    {
        return poller_;
    }

    bool NetworkRuntime::owns_loop() const noexcept
    {
        return owns_;
    }

    EventLoopExitReason NetworkRuntime::run()
    {
        if (loop_) {
            return loop_->loop();
        }
        return EventLoopExitReason::quit_requested;
    }

    void NetworkRuntime::stop()
    {
        if (loop_) {
            loop_->quit();
        }
    }

    timer::Timer *NetworkRuntime::schedule(uint32_t delay_ms, std::function<void()> callback)
    {
        if (!timer_manager_ || !callback) {
            return nullptr;
        }
        return timer::TimerUtil::build_timeout_timer(timer_manager_, delay_ms, [cb = std::move(callback)](timer::Timer *) {
            cb();
        });
    }

    timer::Timer *NetworkRuntime::schedule_periodic(uint32_t delay_ms, uint32_t interval_ms, std::function<void()> callback, int repeat)
    {
        if (!timer_manager_ || !callback) {
            return nullptr;
        }
        return timer::TimerUtil::build_period_timer(timer_manager_, delay_ms, interval_ms, [cb = std::move(callback)](timer::Timer *) {
            cb();
                                                                                           },
                                                    repeat);
    }

    void NetworkRuntime::cancel_timer(timer::Timer * timer)
    {
        if (timer) {
            timer->cancel();
        }
    }

    void NetworkRuntime::dispatch(std::function<void()> callback)
    {
        if (loop_ && callback) {
            loop_->run_in_loop(std::move(callback));
        }
    }

    void NetworkRuntime::register_connection(Connection * conn, ConnectionHandler * handler)
    {
        if (!loop_ || !conn) {
            return;
        }

        loop_->on_new_connection(conn);
        conn->set_connection_handler(handler);
        conn->set_event_handler(loop_);
    }

    void NetworkRuntime::update_channel(Channel * channel)
    {
        if (loop_ && channel) {
            loop_->update_channel(channel);
        }
    }

    void NetworkRuntime::register_connector(Connector * connector, ConnectorHandler * handler)
    {
        if (!connector || !loop_ || !timer_manager_) {
            return;
        }
        connector->set_data(timer_manager_, handler, loop_);
    }

    void NetworkRuntime::register_acceptor(Acceptor * acceptor, ConnectionHandler * handler, Channel * channel)
    {
        if (!acceptor || !loop_) {
            return;
        }
        acceptor->set_event_handler(loop_);
        acceptor->set_connection_handler(handler);
        if (channel) {
            loop_->update_channel(channel);
        }
    }

} // namespace yuan::net
