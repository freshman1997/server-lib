#include <cassert>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <unistd.h>

#include "net/base/channel/channel.h"
#include "net/base/event/event_loop.h"
#include "net/base/poller/poller.h"
#include "net/base/connection/connection.h"
#include "timer/timer_manager.h"
#include "net/base/socket/inet_address.h"
#include "net/base/acceptor/acceptor.h"
#include "base/time.h"

namespace helper 
{
    std::mutex m;
    std::condition_variable cond;
}

namespace net 
{
    EventLoop::EventLoop(Poller *_poller, timer::TimerManager *timer_manager) : poller_(_poller), timer_manager_(timer_manager), quit_(false), is_waiting_(false)
    {
    }

    EventLoop::~EventLoop()
    {
    }

    void EventLoop::loop()
    {
        assert(poller_);

        uint32_t from = base::time::get_tick_count();
        while (!quit_) {
            uint32_t to = poller_->poll(2);
            if (to - from >= timer_manager_->get_time_unit()) {
                from = to;
                timer_manager_->tick();
            }

            /*if (to - from < timer_manager_->get_time_unit()) {
                {
                    std::unique_lock<std::mutex> lock(helper::m);
                    auto now = std::chrono::system_clock::now();
                    is_waiting_ = true;
                    helper::cond.wait_until(lock, now + std::chrono::milliseconds(timer_manager_->get_time_unit() - (to - from)));
                    is_waiting_ = false;
                }
            }*/
        }
    }

    void EventLoop::on_new_connection(Connection *conn)
    {
        if (conn) {
            const InetAddress &addr = conn->get_remote_address();
            Channel * channel = conn->get_channel();

            std::cout << "new connection, ip: " << addr.get_ip() << ", port: " << addr.get_port() << ", fd: " << channel->get_fd()<< std::endl;
            
            auto it = channels_.find(channel->get_fd());
            if (it != channels_.end()) {
                int new_fd = ::dup(channel->get_fd());
                assert(new_fd > 0);
                channel->set_new_fd(new_fd);
            }
            
            poller_->update_channel(channel);
            channels_[channel->get_fd()] = channel;

            conn->set_connection_handler(connHandler_);
        }
    }

    void EventLoop::on_quit()
    {
        quit_ = true;
    }

    void EventLoop::on_close(Connection *conn)
    {
        if (!conn) {
            return;
        }

        Channel * channel = conn->get_channel();
        auto it = channels_.find(channel->get_fd());
        if (it != channels_.end()) {
            poller_->remove_channel(channel);
            channels_.erase(it);
        }
    }

    bool EventLoop::is_unique(int fd)
    {
        auto it = channels_.find(fd);
        return it == channels_.end();
    }

    void EventLoop::update_event(Channel *channel)
    {
        if (channel) {
            poller_->update_channel(channel);
        }
    }

    void EventLoop::wakeup()
    {
        helper::cond.notify_all();
    }
}