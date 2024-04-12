#include <cstring>
#include <set>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>

#include "base/time.h"
#include "net/base/poller/epoll_poller.h"
#include "net/base/channel/channel.h"

namespace net::helper 
{
    std::set<int> fds_;
}

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

    uint64_t EpollPoller::poll(uint32_t timeout)
    {
        uint64_t tm = base::time::get_tick_count();
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
        auto it = helper::fds_.find(channel->get_fd());
        if (it != helper::fds_.end()) {
            if (!channel->has_events()) {
                remove_channel(channel);
            } else {
                update(EPOLL_CTL_MOD, channel);
            }
        } else {
            update(EPOLL_CTL_ADD, channel);
        }
    }

    void EpollPoller::remove_channel(Channel *channel)
    {
        update(EPOLL_CTL_DEL, channel);
        helper::fds_.erase(channel->get_fd());
    }

    void EpollPoller::update(int op, Channel *channel)
    {
        struct epoll_event event;
        memset(&event, 0, sizeof(struct epoll_event));

        event.events |= EPOLLET;
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