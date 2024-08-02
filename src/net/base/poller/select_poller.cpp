#include "base/time.h"
#include "net/base/channel/channel.h"
#include "net/base/poller/select_poller.h"

#include <map>
#include <vector>

namespace net
{
    class SelectPoller::HelperData
    {
    public:
        fd_set reads_;
        fd_set writes_;
		fd_set excepts_;
        std::map<int, net::Channel *> sockets_;
        std::vector<int> removed_fds_;
    };

    SelectPoller::SelectPoller() : data_(std::make_unique<HelperData>())
    {
        
    }

    SelectPoller::~SelectPoller()
    {
        
    }

    uint64_t SelectPoller::poll(uint32_t timeout)
    {
        uint64_t tm = base::time::get_tick_count();

        if (!data_->removed_fds_.empty()) {
            for (auto &fd : data_->removed_fds_) {
                data_->sockets_.erase(fd);
            }
            data_->removed_fds_.clear();
        }

        FD_ZERO(&data_->reads_);
        FD_ZERO(&data_->writes_);
        FD_ZERO(&data_->excepts_);

        int max_fd = 0;
        for (auto i = data_->sockets_.begin(); i != data_->sockets_.end(); ++i) {
            if (i->second->get_events() & Channel::READ_EVENT) {
                FD_SET(i->first, &data_->reads_);
                if (i->first > max_fd) {
                    max_fd = i->first;
                }
            }

            if (i->second->get_events() & Channel::READ_EVENT) {
                FD_SET(i->first, &data_->writes_);
                if (i->first > max_fd) {
                    max_fd = i->first;
                }
            }

            if (i->second->get_events() & Channel::EXCEP_EVENT) {
                FD_SET(i->first, &data_->excepts_);
                if (i->first > max_fd) {
                    max_fd = i->first;
                }
            }
        }

        struct timeval tv;
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;

        int ret = select(max_fd + 1, &data_->reads_, &data_->writes_, &data_->excepts_, &tv);
        if (ret <= 0) {
            // TODO
            return tm;
        }

        for (auto j = data_->sockets_.begin(); j != data_->sockets_.end();) {
            if (!j->second) {
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

            if (ev != Channel::NONE_EVENT) {
                j->second->on_event(ev);
            }
            
            ++j;
        }

        return tm;
    }

    void SelectPoller::update_channel(Channel *channel)
    {
        if (!channel->has_events()) {
            data_->removed_fds_.push_back(channel->get_fd());
            data_->sockets_[channel->get_fd()] = nullptr;
        } else {
            data_->sockets_[channel->get_fd()] = channel;
        }
    }

    void SelectPoller::remove_channel(Channel *channel)
    {
        data_->removed_fds_.push_back(channel->get_fd());
        data_->sockets_[channel->get_fd()] = nullptr;
    }
}