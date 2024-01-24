#include <algorithm>
#include <sys/poll.h>
#include <unordered_map>
#include <vector>
#include <poll.h>

#include "net/poller/poll_poller.h"
#include "net/channel/channel.h"

namespace  
{
    namespace helper 
    {
        std::vector<struct pollfd> fds_;
        std::unordered_map<int, net::Channel *> channels_;
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
        channel->set_oper(Channel::Oper::add);
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
        int fd = channel->get_fd();
        if (channel->get_oper() == Channel::Oper::init || channel->get_oper() == Channel::Oper::free) {
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
        helper::channels_.erase(channel->get_fd());
        channel->set_oper(Channel::Oper::free);
        int rm_fd = channel->get_fd();
        auto it = std::remove_if(helper::fds_.begin(), helper::fds_.end(), [rm_fd](struct pollfd pfd) -> bool {
            return rm_fd == pfd.fd;
        });

        helper::fds_.erase(it);
    }
}