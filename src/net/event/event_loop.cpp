#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#include <time.h>
#include <unistd.h>

#include "net/channel/channel.h"
#include "net/event/event_loop.h"
#include "net/poller/poller.h"
#include "net/connection/connection.h"
#include "timer/timer_manager.h"
#include "net/socket/inet_address.h"
#include "net/acceptor/acceptor.h"
#include "base/time.h"

namespace helper 
{
    std::mutex m;
    std::condition_variable cond;
}

namespace net 
{
    EventLoop::EventLoop(Poller *_poller, timer::TimerManager *timer_manager) : poller_(_poller), timer_manager_(timer_manager), quit_(false)
    {
    }

    EventLoop::~EventLoop()
    {
    }

    void EventLoop::loop()
    {
        assert(poller_);

        while (!quit_) {
            uint32_t from = base::time::get_tick_count();
            time_t tm = poller_->poll(100);
            timer_manager_->tick();
            uint32_t to = base::time::get_tick_count();
            if (to - from < 100) {
                {
                    std::unique_lock<std::mutex> lock(helper::m);
                    auto now = std::chrono::system_clock::now();
                    helper::cond.wait_until(lock, now + std::chrono::milliseconds(100 - (to - from)));
                }
            }
        }
    }

    void EventLoop::on_new_connection(Connection *conn, Acceptor *acceptor)
    {
        if (conn) {
            const InetAddress &addr = conn->get_remote_address();
            Channel * channel = conn->get_channel();

            std::cout << "new connection, ip: " << addr.get_ip() << ", port: " << addr.get_port() << ", fd: " << channel->get_fd()<< std::endl;
            
            auto it = channels_.find(channel->get_fd());
            if (it != channels_.end()) {
                int new_fd = ::dup(channel->get_fd());
                if (new_fd < 0) {
                    assert(0);
                }

                channel->set_new_fd(new_fd);
            }

            conn->set_connection_handler(connHandler_);
            poller_->update_channel(channel);
            channels_[channel->get_fd()] = channel;
        }
    }

    void EventLoop::on_quit(Acceptor *acceptor)
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
            ::close(channel->get_fd());
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