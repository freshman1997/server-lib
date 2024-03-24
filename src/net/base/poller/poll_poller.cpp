#include <algorithm>
#include <sys/poll.h>
#include <unordered_map>
#include <vector>
#include <poll.h>

#include "net/base/poller/poll_poller.h"
#include "net/base/channel/channel.h"

namespace  
{
    namespace helper 
    {
        std::vector<struct pollfd> fds_;
        std::unordered_map<int, net::Channel *> channels_;
        std::vector<int> removed_fds_;
    }
}

namespace net
{
    PollPoller::PollPoller()
    {

    }

    time_t PollPoller::poll(int timeout)
    {
        time_t tm = time(nullptr);

        if (!helper::removed_fds_.empty()) {
            for (auto &fd : helper::removed_fds_) {
                auto it = helper::channels_.find(fd);
                if (it != helper::channels_.end()) {
                    helper::channels_.erase(fd);
                    helper::fds_.erase(std::remove_if(helper::fds_.begin(), helper::fds_.end(), 
                    [fd](struct pollfd pfd) -> bool {
                        return fd == pfd.fd;
                    }));
                }
            }

            helper::removed_fds_.clear();
        }

        int ret = ::poll(&*helper::fds_.begin(), helper::fds_.size(), timeout);
        if (ret < 0) {
            // TODO
            return tm;
        }

        for (int i = 0; i < helper::fds_.size(); ++i) {
            int ev = Channel::NONE_EVENT;
            if (helper::fds_[i].revents & POLLRDHUP || helper::fds_[i].revents & POLLERR 
                || helper::fds_[i].revents & POLLIN) {
                ev |= Channel::READ_EVENT;
            }
            
            if (helper::fds_[i].revents & POLLOUT) {
                ev |= Channel::WRITE_EVENT;
                helper::fds_[i].events &= (~POLLOUT);
                helper::fds_[i].events |= POLLIN;
            }

            if (ev != Channel::NONE_EVENT) {
                Channel *channel = helper::channels_[helper::fds_[i].fd];
                channel->on_event(ev);
            }
        }

        return tm;
    }

    static void do_add_channel(Channel *channel)
    {
        helper::channels_[channel->get_fd()] = channel;
        struct pollfd pfd;
        pfd.revents = 0;
        if (channel->get_events() & Channel::READ_EVENT) {
            pfd.events |= POLLIN | POLLERR;
        }

        if (channel->get_events() & Channel::READ_EVENT) {
            pfd.events |= POLLOUT;
        }

        helper::fds_.push_back(pfd);
    }

    void PollPoller::update_channel(Channel *channel)
    {
        auto it = helper::channels_.find(channel->get_fd());
        if (it == helper::channels_.end()) {
            do_add_channel(channel);
        } else {
            if (!channel->has_events()) {
                remove_channel(channel);
            } else {
                int fd = channel->get_fd();
                auto it = std::find_if(helper::fds_.begin(), helper::fds_.end(), [fd](const struct pollfd &pfd) -> bool {
                    return pfd.fd == fd;
                });

                helper::channels_[channel->get_fd()] = channel;
                if (it != helper::fds_.end()) {
                    helper::channels_[channel->get_fd()] = channel;
                    if (channel->get_events() & Channel::READ_EVENT) {
                        it->events |= POLLIN | POLLERR;
                    }

                    if (channel->get_events() & Channel::READ_EVENT) {
                        it->events |= POLLOUT;
                    }
                } else {
                    do_add_channel(channel);
                }
            }
        }
    }

    void PollPoller::remove_channel(Channel *channel)
    {
        helper::removed_fds_.push_back(channel->get_fd());
    }
}