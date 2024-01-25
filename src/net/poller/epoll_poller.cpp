#include <cstring>
#include <ctime>
#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>

#include "net/poller/epoll_poller.h"
#include "net/channel/channel.h"

namespace net 
{
    const int EpollPoller::MAX_EVENT = 4096;

    EpollPoller::EpollPoller()
    {
        //signal(SIGPIPE, SIG_IGN);
        epoll_fd_ = ::epoll_create(65535);
        // 设置 ET 触发模式 ？
        epoll_events_.resize(10);
    }
    
    EpollPoller::~EpollPoller()
    {
        ::close(epoll_fd_);
    }

    time_t EpollPoller::poll(int timeout)
    {
        time_t tm = time(nullptr);
        int nevent = ::epoll_wait(epoll_fd_, &*epoll_events_.begin(), (int)epoll_events_.size(), timeout);
        if (nevent < 0) {
            return tm;
        }

        if (nevent > 0) {
            for (int i = 0; i < nevent; ++i) {
                Channel *channel = static_cast<Channel *>(epoll_events_[i].data.ptr);
                int ev = Channel::NONE_EVENT;
                int event = epoll_events_[i].events;
                if (event & EPOLLIN || event & EPOLLERR || event & EPOLLHUP) {
                    ev |= Channel::READ_EVENT;
                }

                if (event & EPOLLOUT) {
                    ev |= Channel::WRITE_EVENT;
                }

                channel->on_event(ev);
            }
            
            if (nevent == (int)epoll_events_.size() && (int)epoll_events_.size() < MAX_EVENT) {
                epoll_events_.resize(epoll_events_.size() * 2 >= MAX_EVENT ? MAX_EVENT : epoll_events_.size() * 2);
            }
        } else {
            // TODO log
        }

        return tm;
    }

    void EpollPoller::update_channel(Channel *channel)
    {
        if (channel->get_oper() == Channel::Oper::init || channel->get_oper() == Channel::Oper::free) {
            channel->set_oper(Channel::Oper::add);
            update(EPOLL_CTL_ADD, channel);
        } else {
            if (!channel->has_events()) {
                update(EPOLL_CTL_DEL, channel);
                channel->set_oper(Channel::Oper::free);
            } else {
                update(EPOLL_CTL_MOD, channel);
            }
        }
    }

    void EpollPoller::remove_channel(Channel *channel)
    {
        update(EPOLL_CTL_DEL, channel);
        channel->set_oper(Channel::Oper::free);
    }

    void EpollPoller::update(int op, Channel *channel)
    {
        struct epoll_event event;
        memset(&event, 0, sizeof(struct epoll_event));
        int ev = channel->get_events();
        if (ev & Channel::READ_EVENT) {
            event.events |= EPOLLIN | EPOLLERR | EPOLLHUP;
        }

        if (ev & Channel::WRITE_EVENT) {
            event.events |= EPOLLOUT;
        }

        event.data.ptr = channel;

        if (::epoll_ctl(epoll_fd_, op, channel->get_fd(), &event)) {
            // log
        }
    }
}