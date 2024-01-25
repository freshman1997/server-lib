#include <cassert>
#include <iostream>
#include <time.h>
#include <unistd.h>

#include "net/channel/channel.h"
#include "net/event/event_loop.h"
#include "net/poller/poller.h"
#include "net/connection/connection.h"
#include "timer/timer_manager.h"
#include "net/socket/inet_address.h"
#include "net/acceptor/acceptor.h"

namespace net 
{

    EventLoop::EventLoop(Poller *_poller, timer::TimerManager *timer_manager, Acceptor *acceptor) : poller_(_poller), timer_manager_(timer_manager), quit_(false)
    {
        Channel * channel = acceptor->get_channel();
        channel->enable_read();
        channel->enable_write();
        channels_[channel->get_fd()] = channel;
        poller_->update_channel(channel);
    }

    EventLoop::~EventLoop()
    {
    }

    void EventLoop::loop()
    {
        assert(poller_);

        while (!quit_) {
            time_t from = poller_->poll(100);
            timer_manager_->tick();
            //time_t to = time(NULL);
            //std::cout << "comsume: " << (to - from) << std::endl;
        }
    }

    void EventLoop::on_new_connection(Connection *conn, Acceptor *acceptor)
    {
        if (conn) {
            const InetAddress &addr = conn->get_remote_address();
            Channel * channel = conn->get_channel();

            std::cout << "new connection, ip: " << addr.get_ip() << ", port: " << addr.get_port() << ", fd: " << channel->get_fd()<< std::endl;

            channel->enable_read();
            channel->enable_write();
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
            std::cout << "close connection now: " << channel->get_fd() << "\n";
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
}