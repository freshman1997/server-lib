#include <cassert>
#include <cstddef>
#include <iostream>
#include <time.h>

#include "net/channel/channel.h"
#include "net/event/event_loop.h"
#include "net/poller/poller.h"
#include "net/connection/connection.h"
#include "timer/timer_manager.h"
#include "net/socket/inet_address.h"
#include "net/acceptor/acceptor.h"

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
            time_t from = poller_->poll(100);
            time_t to = time(NULL);
            timer_manager_->tick();
            std::cout << "comsume: " << (to - from) << std::endl;
        }
    }

    void EventLoop::start()
    {
        // TODO 
    }

    void EventLoop::on_new_connection(Connection *conn, Acceptor *acceptor)
    {
        if (conn) {
            const InetAddress &addr = conn->get_remote_address();
            std::cout << "new connection, ip: " << addr.get_ip() << ", port: " << addr.get_port() << std::endl;

            Channel * channel = conn->get_channel();
            channel->enable_read();
            channel->enable_write();
            channels_[channel->get_fd()] = channel;
            poller_->update_channel(channel);
        } else {
            Channel * channel = acceptor->get_channel();
            channel->enable_read();
            channels_[channel->get_fd()] = channel;
            poller_->update_channel(channel);
        }
    }

    void EventLoop::on_close(Acceptor *acceptor)
    {
        quit_ = true;
    }
}