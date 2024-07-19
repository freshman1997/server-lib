#include "base/time.h"
#include "net/base/channel/channel.h"
#include "net/base/poller/select_poller.h"

namespace net
{
    SelectPoller::SelectPoller()
    {
        
    }

    SelectPoller::~SelectPoller()
    {
        
    }

    uint64_t SelectPoller::poll(uint32_t timeout)
    {
        uint64_t tm = base::time::get_tick_count();

        if (!removed_fds_.empty()) {
            for (auto &fd : removed_fds_) {
                sockets_.erase(fd);
            }
            removed_fds_.clear();
        }

        FD_ZERO(&reads_);
        FD_ZERO(&writes_);
        FD_ZERO(&excepts_);

        int max_fd = 0;
        for (auto i = sockets_.begin(); i != sockets_.end(); ++i) {
            if (i->second->get_events() & Channel::READ_EVENT) {
                FD_SET(i->first, &reads_);
                if (i->first > max_fd) {
                    max_fd = i->first;
                }
            }

            if (i->second->get_events() & Channel::READ_EVENT) {
                FD_SET(i->first, &writes_);
                if (i->first > max_fd) {
                    max_fd = i->first;
                }
            }

            if (i->second->get_events() & Channel::EXCEP_EVENT) {
                FD_SET(i->first, &excepts_);
                if (i->first > max_fd) {
                    max_fd = i->first;
                }
            }
        }

        struct timeval tv;
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;

        int ret = select(max_fd + 1, &reads_, &writes_, &excepts_, &tv);
        if (ret <= 0) {
            // TODO
            return tm;
        }

        for (auto j = sockets_.begin(); j != sockets_.end();) {
            if (!j->second) {
                j = sockets_.erase(j);
                continue;
            }

            int ev = Channel::NONE_EVENT;
            if (FD_ISSET(j->first, &reads_)) {
                ev |= Channel::READ_EVENT;
            }

            if (FD_ISSET(j->first, &writes_)) {
                ev |= Channel::WRITE_EVENT;
            }

            if (FD_ISSET(j->first, &excepts_)) {
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
            removed_fds_.push_back(channel->get_fd());
            sockets_[channel->get_fd()] = nullptr;
        } else {
            sockets_[channel->get_fd()] = channel;
        }
    }

    void SelectPoller::remove_channel(Channel *channel)
    {
        removed_fds_.push_back(channel->get_fd());
        sockets_[channel->get_fd()] = nullptr;
    }
}