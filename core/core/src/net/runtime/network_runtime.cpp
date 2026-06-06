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
#include "net/poller/poll_poller.h"
#include "net/poller/select_poller.h"
#include "base/owner_ptr.h"
#include "timer/timer_manager_factory.h"

namespace yuan::net
{

    NetworkRuntime::NetworkRuntime()
        : NetworkRuntime(timer::TimerBackend::automatic)
    {
    }

    NetworkRuntime::NetworkRuntime(timer::TimerBackend timer_backend)
        : owns_(true)
    {
        owned_poller_.reset(create_default_poller());
        owned_poller_->init();
        owned_timer_manager_ = timer::create_timer_manager(timer_backend);
        owned_loop_.reset(new EventLoop(yuan::base::owner_ptr(owned_poller_), yuan::base::owner_ptr(owned_timer_manager_)));

        poller_ = yuan::base::owner_ptr(owned_poller_);
        timer_manager_ = yuan::base::owner_ptr(owned_timer_manager_);
        loop_ = yuan::base::owner_ptr(owned_loop_);
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
#if defined(__linux__)
        return new EpollPoller;
#elif defined(__APPLE__)
        return new KQueuePoller;
#else
        return new PollPoller;
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

    timer::TimerHandle NetworkRuntime::schedule(uint32_t delay_ms, std::function<void()> callback)
    {
        if (!timer_manager_ || !callback) {
            return {};
        }
        return timer_manager_->after(delay_ms, [cb = std::move(callback)]() {
            cb();
        });
    }

    timer::TimerHandle NetworkRuntime::schedule_periodic(uint32_t delay_ms, uint32_t interval_ms, std::function<void()> callback, int repeat)
    {
        if (!timer_manager_ || !callback) {
            return {};
        }
        return timer_manager_->every(delay_ms, interval_ms, [cb = std::move(callback)]() {
            cb();
        }, repeat);
    }

    void NetworkRuntime::dispatch(std::function<void()> callback)
    {
        if (loop_ && callback) {
            loop_->queue_in_loop(std::move(callback));
        }
    }

    void NetworkRuntime::register_connection(const std::shared_ptr<Connection> &conn, std::shared_ptr<ConnectionHandler> handler)
    {
        if (!loop_ || !conn) {
            return;
        }

        conn->set_connection_handler(std::move(handler));
        conn->set_event_handler(loop_);
        loop_->on_new_connection(conn);
    }

    void NetworkRuntime::register_connection(Connection * conn, std::shared_ptr<ConnectionHandler> handler)
    {
        if (!loop_ || !conn) {
            return;
        }

        conn->set_connection_handler(std::move(handler));
        conn->set_event_handler(loop_);
        loop_->on_new_connection(conn->shared_from_this());
    }

    void NetworkRuntime::update_channel(Channel * channel)
    {
        if (loop_ && channel) {
            loop_->update_channel(channel);
        }
    }

    void NetworkRuntime::register_connector(Connector * connector, std::shared_ptr<ConnectorHandler> handler)
    {
        if (!connector || !loop_ || !timer_manager_) {
            return;
        }
        connector->set_data(timer_manager_, std::move(handler), loop_);
    }

    void NetworkRuntime::register_acceptor(Acceptor * acceptor, std::shared_ptr<ConnectionHandler> handler, Channel * channel)
    {
        if (!acceptor || !loop_) {
            return;
        }
        acceptor->set_event_handler(loop_);
        acceptor->set_connection_handler(std::move(handler));
        if (channel) {
            loop_->update_channel(channel);
        }
    }

} // namespace yuan::net
