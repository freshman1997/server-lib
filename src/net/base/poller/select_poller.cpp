#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <map>
#include <vector>

#include "base/time.h"
#include "net/base/channel/channel.h"
#include "net/base/poller/select_poller.h"

namespace 
{
    namespace helper 
    {
        std::map<int, net::Channel *> sockets_;
        std::vector<int> removed_fds_;
        fd_set reads_;
        fd_set writes_;
		fd_set excepts_;
    }
}

namespace net
{
    SelectPoller::SelectPoller()
    {
        #ifdef _WIN32
            WSADATA data;
            WSAStartup(MAKEWORD(2, 2), &data);
        #endif
        
        FD_ZERO(&helper::reads_);
        FD_ZERO(&helper::writes_);
    }

    uint32_t SelectPoller::poll(uint32_t timeout)
    {
        uint32_t tm = base::time::get_tick_count();

        if (!helper::removed_fds_.empty()) {
            for (auto &fd : helper::removed_fds_) {
                auto it = helper::sockets_.find(fd);
                if (it != helper::sockets_.end()) {
                    helper::sockets_.erase(it);
                }
            }

            helper::removed_fds_.clear();
        }

        FD_ZERO(&helper::reads_);
        FD_ZERO(&helper::writes_);
        FD_ZERO(&helper::excepts_);

        int max_fd = 0;
        for (auto i = helper::sockets_.begin(); i != helper::sockets_.end(); ++i) {
            if (i->second->get_events() & Channel::READ_EVENT) {
                FD_SET(i->first, &helper::reads_);
                if (i->first > max_fd) {
                    max_fd = i->first;
                }
            }

            if (i->second->get_events() & Channel::READ_EVENT) {
                FD_SET(i->first, &helper::writes_);
                if (i->first > max_fd) {
                    max_fd = i->first;
                }
            }

            if (i->second->get_events() & Channel::EXCEP_EVENT) {
                FD_SET(i->first, &helper::excepts_);
                if (i->first > max_fd) {
                    max_fd = i->first;
                }
            }
        }

        struct timeval tv;
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;

        int ret = select(max_fd + 1, &helper::reads_, &helper::writes_, &helper::excepts_, &tv);
        if (ret <= 0) {
            // TODO
            return tm;
        }

        for (auto j = helper::sockets_.begin(); j != helper::sockets_.end(); ++j) {
            int ev = Channel::NONE_EVENT;
            if (FD_ISSET(j->first, &helper::reads_)) {
                ev |= Channel::READ_EVENT;
            }

            if (FD_ISSET(j->first, &helper::writes_)) {
                ev |= Channel::WRITE_EVENT;
            }

            if (FD_ISSET(j->first, &helper::excepts_)) {
                ev |= Channel::EXCEP_EVENT;
            }

            if (ev != Channel::NONE_EVENT) {
                j->second->on_event(ev);
            }
        }

        return tm;
    }

    void SelectPoller::update_channel(Channel *channel)
    {
        if (!channel->has_events()) {
            helper::removed_fds_.push_back(channel->get_fd());
        } else {
            helper::sockets_[channel->get_fd()] = channel;
        }
    }

    void SelectPoller::remove_channel(Channel *channel)
    {
        helper::removed_fds_.push_back(channel->get_fd());
    }
}