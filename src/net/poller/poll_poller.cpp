#ifdef __linux__
#include <algorithm>
#include <sys/poll.h>
#include <unordered_map>
#include <vector>
#include <poll.h>

#include "base/time.h"
#include "net/poller/poll_poller.h"
#include "net/channel/channel.h"

namespace net
{
    class PollPoller::HelperData
    {
    public:
        std::vector<struct pollfd> fds_;
        std::unordered_map<int, net::Channel *> channels_;
        std::vector<int> removed_fds_;
    };

    PollPoller::PollPoller() : data_(std::make_unique<PollPoller::HelperData>())
    {
        
    }

    PollPoller::~PollPoller()
    {
        
    }

    bool PollPoller::init()
    {
        return true;
    }

    uint64_t PollPoller::poll(uint32_t timeout, std::vector<Channel *> &channels)
    {
        uint64_t tm = base::time::get_tick_count();

        if (!data_->removed_fds_.empty()) {
            for (auto &fd : data_->removed_fds_) {
                auto it = data_->channels_.find(fd);
                if (it != data_->channels_.end()) {
                    data_->channels_.erase(fd);
                    data_->fds_.erase(std::remove_if(data_->fds_.begin(), data_->fds_.end(), 
                    [fd](struct pollfd pfd) -> bool {
                        return fd == pfd.fd;
                    }));
                }
            }

            data_->removed_fds_.clear();
        }

        int ret = ::poll(&*data_->fds_.begin(), data_->fds_.size(), timeout);
        if (ret < 0) {
            // TODO
            return tm;
        }

        if (channels.size() != data_->fds_.size()) {
            channels.reserve(data_->fds_.size());
        }

        for (int i = 0; i < data_->fds_.size(); ++i) {
            channels[i] = nullptr;
            int ev = Channel::NONE_EVENT;
            if (data_->fds_[i].revents & POLLRDHUP || data_->fds_[i].revents & POLLERR 
                || data_->fds_[i].revents & POLLIN) {
                ev |= Channel::READ_EVENT;
            }
            
            if (data_->fds_[i].revents & POLLOUT) {
                ev |= Channel::WRITE_EVENT;
            }

            Channel *channel = data_->channels_[data_->fds_[i].fd];
            if (ev != Channel::NONE_EVENT && channel) {
                channel->set_revent(ev);
                channels.push_back(channel);
            }
        }

        return tm;
    }

    void PollPoller::do_add_channel(Channel *channel)
    {
        data_->channels_[channel->get_fd()] = channel;
        struct pollfd pfd;
        pfd.revents = 0;
        if (channel->get_events() & Channel::READ_EVENT) {
            pfd.events |= POLLIN | POLLERR;
        }

        if (channel->get_events() & Channel::READ_EVENT) {
            pfd.events |= POLLOUT;
        }

        data_->fds_.push_back(pfd);
    }

    void PollPoller::update_channel(Channel *channel)
    {
        auto it = data_->channels_.find(channel->get_fd());
        if (it == data_->channels_.end()) {
            do_add_channel(channel);
        } else {
            if (!channel->has_events()) {
                remove_channel(channel);
            } else {
                int fd = channel->get_fd();
                auto it = std::find_if(data_->fds_.begin(), data_->fds_.end(), [fd](const struct pollfd &pfd) -> bool {
                    return pfd.fd == fd;
                });

                data_->channels_[channel->get_fd()] = channel;
                if (it != data_->fds_.end()) {
                    data_->channels_[channel->get_fd()] = channel;
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
        data_->removed_fds_.push_back(channel->get_fd());
        data_->channels_[channel->get_fd()] = nullptr;
    }
}
#endif