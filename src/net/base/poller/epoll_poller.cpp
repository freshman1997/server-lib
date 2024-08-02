#ifdef __linux__
#include <cstring>
#include <set>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <set>
#include <vector>

#include "base/time.h"
#include "net/base/poller/epoll_poller.h"
#include "net/base/channel/channel.h"

namespace net 
{
    class EpollPoller::HelperData 
    {
    public:
        int epoll_fd_;
        std::set<int> fds_;
        std::vector<struct epoll_event> epoll_events_;
    };

    const int EpollPoller::MAX_EVENT = 4096;

    EpollPoller::EpollPoller()
    {
        data_ = std::make_unique<HelperData>();
        //signal(SIGPIPE, SIG_IGN);
        data_->epoll_fd_ = ::epoll_create(65535);
        // 设置 ET 触发模式 ？
        data_->epoll_events_.resize(10);
    }
    
    EpollPoller::~EpollPoller()
    {
        ::close(epoll_fd_);
    }

    uint64_t EpollPoller::poll(uint32_t timeout)
    {
        uint64_t tm = base::time::get_tick_count();
        int nevent = ::epoll_wait(epoll_fd_, &*data_->epoll_events_.begin(), (int)data_->epoll_events_.size(), timeout);
        if (nevent < 0) {
            return tm;
        }

        if (nevent > 0) {
            for (int i = 0; i < nevent; ++i) {
                Channel *channel = static_cast<Channel *>(data_->epoll_events_[i].data.ptr);
                int ev = Channel::NONE_EVENT;
                int event = data_->epoll_events_[i].events;
                if (event & EPOLLIN || event & EPOLLERR || event & EPOLLHUP) {
                    ev |= Channel::READ_EVENT;
                }

                if (event & EPOLLOUT) {
                    ev |= Channel::WRITE_EVENT;
                }

                if (ev != Channel::NONE_EVENT) {
                    channel->on_event(ev);
                }
            }
            
            if (nevent == (int)data_->epoll_events_.size() && (int)data_->epoll_events_.size() < MAX_EVENT) {
                data_->epoll_events_.resize(data_->epoll_events_.size() * 2 >= MAX_EVENT ? MAX_EVENT : data_->epoll_events_.size() * 2);
            }
        } else {
            // TODO log
        }

        return tm;
    }

    void EpollPoller::update_channel(Channel *channel)
    {
        auto it = fds_.find(channel->get_fd());
        if (it != fds_.end()) {
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
        fds_.erase(channel->get_fd());
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

        if (::epoll_ctl(data_->epoll_fd_, op, channel->get_fd(), &event)) {
            // log
        }
    }
}
#endif