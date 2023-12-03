#include <cassert>
#include <cstring>
#include <ctime>
#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <signal.h>

#include "net/poller/epoll_poller.h"
#include "net/channel/channel.h"

namespace net 
{
    EpollPoller::EpollPoller(EventLoop *loop) : Poller(loop) 
    {
        signal(SIGPIPE, SIG_IGN);
        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        // 设置 ET 触发模式 ？
        epoll_events_.resize(100);
    }
    
    EpollPoller::~EpollPoller()
    {
        ::close(epoll_fd_);
    }

    time_t EpollPoller::poll(int timeout, std::vector<Channel *> *activeChannels)
    {
        int nevent = ::epoll_wait(epoll_fd_, &*epoll_events_.begin(), (int)epoll_events_.size(), timeout);
        int err = errno;
        time_t tm = time(nullptr);

        if (nevent > 0) {
            for (int i = 0; i < nevent; ++i) {
                Channel *channel = static_cast<Channel *>(epoll_events_[i].data.ptr);
                int fd = channel->get_fd();
                channel->set_read_event(epoll_events_[i].events);
            }
        } else if (nevent == 0) {
            // TODO log
        } else {
            if (err != EINTR) {
                errno = err;
            }
        }

        return tm;
    }

    void EpollPoller::update_channel(Channel *channel)
    {
        int fd = channel->get_fd();
        auto it = channels_.find(fd);
        if (channel->get_oper() == Channel::Oper::init || channel->get_oper() == Channel::Oper::free) {
            if (channel->get_oper() == Channel::Oper::init) {
                if (it != channels_.end()) {
                    assert(0);
                }

                channels_[fd] = channel;
            } 

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
        int fd = channel->get_fd();
        auto it = channels_.find(fd);
        if (it == channels_.end()) {
            return;
        }

        update(EPOLL_CTL_DEL, channel);
        channels_.erase(it);
    }

    void EpollPoller::update(int op, Channel *channel)
    {
        struct epoll_event event;
        memset(&event, 0, sizeof(struct epoll_event));
        event.events = channel->get_events();
        event.data.ptr = channel;

        int fd = channel->get_fd();
        if (::epoll_ctl(epoll_fd_, op, fd, &event)) {
            // log
        }
    }
}