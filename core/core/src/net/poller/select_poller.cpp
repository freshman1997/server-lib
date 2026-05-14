#include "base/time.h"
#include "net/channel/channel.h"
#include "net/poller/select_poller.h"
#include "logger.h"

#include <map>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace yuan::net
{
    class SelectPoller::HelperData
    {
    public:
        fd_set reads_;
        fd_set writes_;
		fd_set excepts_;
        std::map<int, net::Channel *> sockets_;
        std::mutex mutex_;
    };

#ifdef _WIN32
    namespace
    {
        bool is_valid_socket_fd(const int fd)
        {
            if (fd < 0) {
                return false;
            }
            int socket_type = 0;
            int len = static_cast<int>(sizeof(socket_type));
            return ::getsockopt(fd, SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&socket_type), &len) == 0;
        }
    }
#endif

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

        auto queue_close_event = [&events](int fd, Channel *channel) {
            if (!channel) {
                return;
            }
            int revents = channel->get_events() & (Channel::READ_EVENT | Channel::WRITE_EVENT);
            if (revents == Channel::NONE_EVENT) {
                revents = channel->get_events() & Channel::EXCEP_EVENT;
            }
            if (revents == Channel::NONE_EVENT) {
                return;
            }
            PollEvent pe;
            pe.fd = fd;
            pe.revents = revents;
            pe.generation = channel->generation();
            events.push_back(pe);
        };

        FD_ZERO(&data_->reads_);
        FD_ZERO(&data_->writes_);
        FD_ZERO(&data_->excepts_);

        int max_fd = 0;
        bool has_fd = false;
        for (auto i = data_->sockets_.begin(); i != data_->sockets_.end();) {
            if (!i->second) {
                i = data_->sockets_.erase(i);
                continue;
            }

            const int fd = i->first;
            if (fd < 0) {
                LOG_WARN("select poll skip invalid fd: {}", fd);
                queue_close_event(fd, i->second);
                i = data_->sockets_.erase(i);
                continue;
            }

#ifdef _WIN32
            if (!is_valid_socket_fd(fd)) {
                const int err = WSAGetLastError();
                LOG_WARN("select poll remove invalid socket, fd: {}, wsa_error: {}", fd, err);
                queue_close_event(fd, i->second);
                i = data_->sockets_.erase(i);
                continue;
            }
#endif

            if (i->second->get_events() & Channel::READ_EVENT) {
#ifdef _WIN32
                if (data_->reads_.fd_count >= FD_SETSIZE) {
                    LOG_WARN("select poll read set full, FD_SETSIZE: {}, sockets: {}", FD_SETSIZE, data_->sockets_.size());
                    break;
                }
#endif
                FD_SET(fd, &data_->reads_);
                has_fd = true;
                if (fd > max_fd) {
                    max_fd = fd;
                }
            }

            if (i->second->get_events() & Channel::WRITE_EVENT) {
#ifdef _WIN32
                if (data_->writes_.fd_count >= FD_SETSIZE || data_->excepts_.fd_count >= FD_SETSIZE) {
                    LOG_WARN("select poll write/except set full, FD_SETSIZE: {}, sockets: {}", FD_SETSIZE, data_->sockets_.size());
                    break;
                }
#endif
                FD_SET(fd, &data_->writes_);
                FD_SET(fd, &data_->excepts_);
                has_fd = true;
                if (fd > max_fd) {
                    max_fd = fd;
                }
            }

            if (i->second->get_events() & Channel::EXCEP_EVENT) {
#ifdef _WIN32
                if (data_->excepts_.fd_count >= FD_SETSIZE) {
                    LOG_WARN("select poll except set full, FD_SETSIZE: {}, sockets: {}", FD_SETSIZE, data_->sockets_.size());
                    break;
                }
#endif
                FD_SET(fd, &data_->excepts_);
                has_fd = true;
                if (fd > max_fd) {
                    max_fd = fd;
                }
            }

            ++i;
        }

        if (!has_fd) {
            return tm;
        }

        timeval tv{};
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;

        int ret = select(max_fd + 1, &data_->reads_, &data_->writes_, &data_->excepts_, &tv);
        if (ret <= 0) {
            if (ret < 0) {
#ifdef _WIN32
                LOG_WARN("select poll failed, ret: {}, wsa_error: {}, sockets: {}", ret, WSAGetLastError(), data_->sockets_.size());
#else
                LOG_WARN("select poll failed, ret: {}", ret);
#endif
            }
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
                ev |= (j->second->get_events() & Channel::WRITE_EVENT) ? Channel::WRITE_EVENT : Channel::EXCEP_EVENT;
            }

            if (ev != Channel::NONE_EVENT && j->second) {
                PollEvent pe;
                pe.fd = j->first;
                pe.revents = ev;
                pe.generation = j->second->generation();
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
            data_->sockets_[channel->get_fd()] = channel;
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
