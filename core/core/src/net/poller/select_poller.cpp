#include "base/time.h"
#include "net/channel/channel.h"
#include "net/poller/select_poller.h"
#include "logger.h"

#include <map>
#include <mutex>
#include <vector>

namespace yuan::net
{
    class SelectPoller::HelperData
    {
    public:
        struct ChannelEntry
        {
            net::Channel *channel = nullptr;
            uint64_t generation = 0;
        };

        fd_set reads_;
        fd_set writes_;
		fd_set excepts_;
        std::map<int, ChannelEntry> sockets_;
        std::mutex mutex_;
    };

    SelectPoller::SelectPoller() : data_(std::make_unique<HelperData>())
    {
        
    }

    SelectPoller::~SelectPoller()
    {
        
    }

    bool SelectPoller::init()
    {
        return true;
    }

    uint64_t SelectPoller::poll(uint32_t timeout, std::vector<PollEvent> &events)
    {
        uint64_t tm = base::time::get_tick_count();
        std::lock_guard<std::mutex> lock(data_->mutex_);

        FD_ZERO(&data_->reads_);
        FD_ZERO(&data_->writes_);
        FD_ZERO(&data_->excepts_);

        int max_fd = 0;
        for (auto i = data_->sockets_.begin(); i != data_->sockets_.end(); ++i) {
            if (!i->second.channel) {
                continue;
            }

            if (i->second.channel->get_events() & Channel::READ_EVENT) {
                FD_SET(i->first, &data_->reads_);
                if (i->first > max_fd) {
                    max_fd = i->first;
                }
            }

            if (i->second.channel->get_events() & Channel::WRITE_EVENT) {
                FD_SET(i->first, &data_->writes_);
                if (i->first > max_fd) {
                    max_fd = i->first;
                }
            }

            if (i->second.channel->get_events() & Channel::EXCEP_EVENT) {
                FD_SET(i->first, &data_->excepts_);
                if (i->first > max_fd) {
                    max_fd = i->first;
                }
            }
        }

        timeval tv{};
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;

        int ret = select(max_fd + 1, &data_->reads_, &data_->writes_, &data_->excepts_, &tv);
        if (ret <= 0) {
            if (ret < 0) {
                LOG_WARN("select poll failed, ret: {}", ret);
            }
            return tm;
        }

        for (auto j = data_->sockets_.begin(); j != data_->sockets_.end();) {
            if (!j->second.channel) {
                j = data_->sockets_.erase(j);
                continue;
            }

            int ev = Channel::NONE_EVENT;
            if (FD_ISSET(j->first, &data_->reads_)) {
                ev |= Channel::READ_EVENT;
            }

            if (FD_ISSET(j->first, &data_->writes_)) {
                ev |= Channel::WRITE_EVENT;
            }

            if (FD_ISSET(j->first, &data_->excepts_)) {
                ev |= Channel::EXCEP_EVENT;
            }

            if (ev != Channel::NONE_EVENT && j->second.channel) {
                PollEvent pe;
                pe.fd = j->first;
                pe.revents = ev;
                pe.generation = j->second.generation;
                events.push_back(pe);
            }
            
            ++j;
        }

        return tm;
    }

    void SelectPoller::update_channel(Channel *channel)
    {
        if (!channel) {
            return;
        }

        if (!channel->has_events()) {
            remove_channel(channel);
        } else {
            std::lock_guard<std::mutex> lock(data_->mutex_);
            auto &entry = data_->sockets_[channel->get_fd()];
            entry.channel = channel;
        }
    }

    void SelectPoller::remove_channel(Channel *channel)
    {
        if (!channel) {
            return;
        }

        std::lock_guard<std::mutex> lock(data_->mutex_);
        data_->sockets_.erase(channel->get_fd());
    }
}
