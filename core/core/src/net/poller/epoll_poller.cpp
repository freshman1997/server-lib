#ifdef __linux__
#include <cstring>
#include <set>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <set>
#include <vector>

#include "base/time.h"
#include "net/poller/epoll_poller.h"
#include "net/channel/channel.h"

namespace yuan::net 
{
    class EpollPoller::HelperData 
    {
    public:
        HelperData() = default;
        ~HelperData() 
        {
            if (epoll_fd_ != -1) {
                ::close(epoll_fd_);
            }
        }
        
    public:
        int epoll_fd_;
        std::set<int> fds_;
        std::vector<struct epoll_event> epoll_events_;
    };

    const int EpollPoller::MAX_EVENT = 4096;

    EpollPoller::EpollPoller()
    {
        data_ = std::make_unique<EpollPoller::HelperData>();
        //signal(SIGPIPE, SIG_IGN);
        data_->epoll_fd_ = ::epoll_create(65535);
        // 设置 ET 触发模式 ？
        data_->epoll_events_.resize(10);
    }
    
    EpollPoller::~EpollPoller()
    {

    }

    bool EpollPoller::init()
    {
        return data_->epoll_fd_ != -1;
    }

    uint64_t EpollPoller::poll(uint32_t timeout, std::vector<Channel *> &channels)
    {
        int nevent = ::epoll_wait(data_->epoll_fd_, &*data_->epoll_events_.begin(), (int)data_->epoll_events_.size(), timeout);
        uint64_t tm = base::time::get_tick_count();
        if (nevent < 0) {
            return tm;
        }

        if (nevent > 0) {
            for (int i = 0; i < nevent; ++i) {
                int ev = Channel::NONE_EVENT;
                int event = data_->epoll_events_[i].events;
                if (event & EPOLLIN || event & EPOLLERR || event & EPOLLHUP || event & EPOLLRDHUP) {
                    ev |= Channel::READ_EVENT;
                }

                if (event & EPOLLOUT) {
                    ev |= Channel::WRITE_EVENT;
                }

                Channel *channel = static_cast<Channel *>(data_->epoll_events_[i].data.ptr);
                if (ev != Channel::NONE_EVENT && channel) {
                    channel->set_revent(ev);
                    channels.push_back(channel);
                }
            }
            
            if (nevent == (int)data_->epoll_events_.size() && (int)data_->epoll_events_.size() < MAX_EVENT) {
                data_->epoll_events_.resize(data_->epoll_events_.size() * 2 >= MAX_EVENT ? MAX_EVENT : data_->epoll_events_.size() * 2);
            }
        }

        return tm;
    }

    void EpollPoller::update_channel(Channel *channel)
    {
        auto it = data_->fds_.find(channel->get_fd());
        if (it != data_->fds_.end()) {
            if (!channel->has_events()) {
                remove_channel(channel);
            } else {
                update(EPOLL_CTL_MOD, channel);
            }
        } else {
            update(EPOLL_CTL_ADD, channel);
            data_->fds_.insert(channel->get_fd());
        }
    }

    void EpollPoller::remove_channel(Channel *channel)
    {
        update(EPOLL_CTL_DEL, channel);
        data_->fds_.erase(channel->get_fd());
    }

    void EpollPoller::update(int op, Channel *channel)
    {
        int ret = 0;
        if (op != EPOLL_CTL_DEL) {
            struct epoll_event event;
            memset(&event, 0, sizeof(struct epoll_event));

            event.events |= EPOLLET | EPOLLRDHUP;
            int ev = channel->get_events();
            if (ev & Channel::READ_EVENT) {
                event.events |= EPOLLIN | EPOLLERR | EPOLLHUP;
            }

            if (ev & Channel::WRITE_EVENT) {
                event.events |= EPOLLOUT;
            }

            event.data.ptr = channel;

            ret = ::epoll_ctl(data_->epoll_fd_, op, channel->get_fd(), &event);
        } else {
            ret = ::epoll_ctl(data_->epoll_fd_, op, channel->get_fd(), NULL);
        }

        if (ret == -1) {
            // log
        }
    }
}
#endif