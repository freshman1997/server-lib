#ifdef __linux__
#include <algorithm>
#include <sys/poll.h>
#include <unordered_map>
#include <vector>

#include "base/time.h"
#include "net/poller/poll_poller.h"
#include "net/channel/channel.h"
#include "logger.h"

namespace yuan::net
{
    class PollPoller::HelperData
    {
    public:
        std::vector<struct pollfd> fds_;
        std::unordered_map<int, net::Channel *> channels_;
        std::vector<int> removed_fds_;
    };

    PollPoller::PollPoller()
        : data_(std::make_unique<PollPoller::HelperData>())
    {
    }

    PollPoller::~PollPoller()
    {
    }

    bool PollPoller::init()
    {
        return true;
    }

    uint64_t PollPoller::poll(uint32_t timeout, std::vector<PollEvent> & events)
    {
        uint64_t tm = base::time::get_tick_count();

        if (!data_->removed_fds_.empty()) {
            for (auto &fd : data_->removed_fds_) {
                auto it = data_->channels_.find(fd);
                if (it != data_->channels_.end()) {
                    data_->channels_.erase(fd);
                    data_->fds_.erase(std::remove_if(data_->fds_.begin(), data_->fds_.end(),
                                                     [fd](struct pollfd pfd)->bool {
                        return fd == pfd.fd;
                                      }),
                                      data_->fds_.end());
                }
            }

            data_->removed_fds_.clear();
        }

        if (data_->fds_.empty()) {
            return tm;
        }

        int ret = ::poll(data_->fds_.data(), static_cast<nfds_t>(data_->fds_.size()), timeout);
        if (ret < 0) {
            LOG_WARN("poll poller failed, ret: {}", ret);
            return tm;
        }

        for (std::size_t i = 0; i < data_->fds_.size(); ++i) {
            int ev = Channel::NONE_EVENT;
            if (data_->fds_[i].revents & POLLRDHUP || data_->fds_[i].revents & POLLERR || data_->fds_[i].revents & POLLIN) {
                ev |= Channel::READ_EVENT;
            }

            if (data_->fds_[i].revents & POLLOUT) {
                ev |= Channel::WRITE_EVENT;
            }

            const int fd = data_->fds_[i].fd;
            auto it = data_->channels_.find(fd);
            if (it != data_->channels_.end() && ev != Channel::NONE_EVENT && it->second) {
                PollEvent pe;
                pe.fd = fd;
                pe.revents = ev;
                pe.generation = it->second->generation();
                events.push_back(pe);
            }
        }

        return tm;
    }

    void PollPoller::do_add_channel(Channel * channel)
    {
        data_->channels_[channel->get_fd()] = channel;
        struct pollfd pfd;
        pfd.fd = channel->get_fd();
        pfd.events = 0;
        pfd.revents = 0;
        if (channel->get_events() & Channel::READ_EVENT) {
            pfd.events |= POLLIN | POLLERR;
        }

        if (channel->get_events() & Channel::WRITE_EVENT) {
            pfd.events |= POLLOUT;
        }

        data_->fds_.push_back(pfd);
    }

    void PollPoller::update_channel(Channel * channel)
    {
        if (!channel) {
            return;
        }

        auto it = data_->channels_.find(channel->get_fd());
        if (it == data_->channels_.end()) {
            if (channel->has_events()) {
                do_add_channel(channel);
            }
        } else {
            if (!channel->has_events()) {
                remove_channel(channel);
            } else {
                int fd = channel->get_fd();
                auto it = std::find_if(data_->fds_.begin(), data_->fds_.end(), [fd](const struct pollfd & pfd)->bool {
                    return pfd.fd == fd;
                });

                data_->channels_[channel->get_fd()] = channel;
                if (it != data_->fds_.end()) {
                    it->events = 0;
                    it->revents = 0;
                    if (channel->get_events() & Channel::READ_EVENT) {
                        it->events |= POLLIN | POLLERR;
                    }

                    if (channel->get_events() & Channel::WRITE_EVENT) {
                        it->events |= POLLOUT;
                    }
                } else {
                    do_add_channel(channel);
                }
            }
        }
    }

    void PollPoller::remove_channel(Channel * channel)
    {
        if (!channel) {
            return;
        }

        data_->removed_fds_.push_back(channel->get_fd());
        data_->channels_[channel->get_fd()] = nullptr;
    }
}
#endif
