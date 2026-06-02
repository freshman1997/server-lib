#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

#include "base/time.h"
#include "net/poller/poll_poller.h"
#include "net/channel/channel.h"
#include "logger.h"
#include "platform/native_platform.h"

namespace yuan::net
{
    namespace
    {
#ifdef _WIN32
        using NativePollFd = WSAPOLLFD;
        constexpr short READ_MASK = POLLRDNORM;
        constexpr short WRITE_MASK = POLLWRNORM;
        constexpr short REQUEST_MASK = READ_MASK | WRITE_MASK;
        constexpr short ERROR_MASK = POLLERR | POLLHUP | POLLNVAL;
#else
        using NativePollFd = struct pollfd;
        constexpr short READ_MASK = POLLIN;
        constexpr short WRITE_MASK = POLLOUT;
        constexpr short REQUEST_MASK = READ_MASK | WRITE_MASK;
        constexpr short ERROR_MASK = POLLERR | POLLHUP | POLLNVAL;
#ifdef POLLRDHUP
        constexpr short READ_HUP_MASK = POLLRDHUP;
#else
        constexpr short READ_HUP_MASK = 0;
#endif
#endif
    }

    class PollPoller::HelperData
    {
    public:
        void erase_fd(int fd)
        {
            auto index_it = fd_indices_.find(fd);
            if (index_it == fd_indices_.end()) {
                return;
            }

            const std::size_t index = index_it->second;
            const std::size_t last_index = fds_.size() - 1;
            if (index != last_index) {
                fds_[index] = fds_[last_index];
                channels_[index] = channels_[last_index];
                fd_indices_[static_cast<int>(fds_[index].fd)] = index;
            }
            fds_.pop_back();
            channels_.pop_back();
            fd_indices_.erase(index_it);
        }

        std::vector<NativePollFd> fds_;
        std::vector<net::Channel *> channels_;
        std::unordered_map<int, std::size_t> fd_indices_;
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

        if (data_->fds_.empty()) {
            return tm;
        }

        for (auto &pfd : data_->fds_) {
            pfd.revents = 0;
        }

#ifdef _WIN32
        int ret = ::WSAPoll(data_->fds_.data(), static_cast<ULONG>(data_->fds_.size()), static_cast<INT>(timeout));
#else
        int ret = ::poll(data_->fds_.data(), static_cast<nfds_t>(data_->fds_.size()), timeout);
#endif
        if (ret < 0) {
#ifdef _WIN32
            LOG_WARN("poll poller failed, ret: {}, wsa_error: {}, fds: {}", ret, platform::GetLastNativeError(), data_->fds_.size());
#else
            LOG_WARN("poll poller failed, ret: {}", ret);
#endif
            return tm;
        }

        for (std::size_t i = 0; i < data_->fds_.size(); ++i) {
            auto *channel = i < data_->channels_.size() ? data_->channels_[i] : nullptr;
            if (!channel) {
                continue;
            }

            const int fd = static_cast<int>(data_->fds_[i].fd);
            const auto revents = data_->fds_[i].revents;
            const bool has_error = (revents & ERROR_MASK)
#ifndef _WIN32
                || (revents & READ_HUP_MASK)
#endif
                ;

            int ev = Channel::NONE_EVENT;
            if (has_error) {
                ev |= channel->get_events() & (Channel::READ_EVENT | Channel::WRITE_EVENT);
                if (ev == Channel::NONE_EVENT) {
                    ev = Channel::READ_EVENT;
                }
            } else {
                if (revents & READ_MASK) {
                    ev |= Channel::READ_EVENT;
                }

                if (revents & WRITE_MASK) {
                    ev |= Channel::WRITE_EVENT;
                }
            }

            if (ev != Channel::NONE_EVENT) {
                PollEvent pe;
                pe.fd = fd;
                pe.revents = ev;
                pe.generation = channel->generation();
                events.push_back(pe);
            }
        }

        return tm;
    }

    void PollPoller::do_add_channel(Channel * channel)
    {
        NativePollFd pfd{};
        pfd.fd = static_cast<decltype(pfd.fd)>(channel->get_fd());
        pfd.events = 0;
        pfd.revents = 0;
        if (channel->get_events() & Channel::READ_EVENT) {
            pfd.events |= READ_MASK;
        }

        if (channel->get_events() & Channel::WRITE_EVENT) {
            pfd.events |= WRITE_MASK;
        }
        pfd.events &= REQUEST_MASK;

        data_->fds_.push_back(pfd);
        data_->channels_.push_back(channel);
        data_->fd_indices_[channel->get_fd()] = data_->fds_.size() - 1;
    }

    void PollPoller::update_channel(Channel * channel)
    {
        if (!channel) {
            return;
        }

        auto index_it = data_->fd_indices_.find(channel->get_fd());
        if (index_it == data_->fd_indices_.end()) {
            if (channel->has_events()) {
                do_add_channel(channel);
            }
        } else {
            if (!channel->has_events()) {
                remove_channel(channel);
            } else {
                const std::size_t index = index_it->second;
                data_->channels_[index] = channel;
                auto &pfd = data_->fds_[index];
                pfd.events = 0;
                pfd.revents = 0;
                if (channel->get_events() & Channel::READ_EVENT) {
                    pfd.events |= READ_MASK;
                }

                if (channel->get_events() & Channel::WRITE_EVENT) {
                    pfd.events |= WRITE_MASK;
                }
                pfd.events &= REQUEST_MASK;
            }
        }
    }

    void PollPoller::remove_channel(Channel * channel)
    {
        if (!channel) {
            return;
        }

        data_->erase_fd(channel->get_fd());
    }
}
