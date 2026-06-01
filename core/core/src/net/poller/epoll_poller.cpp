#ifdef __linux__
#include <cstring>
#include <cerrno>
#include <set>
#include <unordered_map>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <vector>

#include "base/time.h"
#include "net/poller/epoll_poller.h"
#include "net/channel/channel.h"
#include "native_platform.h"

namespace yuan::net
{
    class EpollPoller::HelperData
    {
    public:
        HelperData() = default;
        ~HelperData()
        {
            if (epoll_fd_ != -1) {
                ::close(epoll_fd_);
            }
        }

    public:
        int epoll_fd_;
        std::set<int> fds_;
        std::unordered_map<int, Channel *> channels_;
        std::vector<struct epoll_event> epoll_events_;
    };

    namespace
    {
        static uint64_t encode_token(int fd, uint64_t generation)
        {
            return (generation << 32) | static_cast<uint32_t>(fd);
        }

        static int decode_fd(uint64_t token)
        {
            return static_cast<int>(token & 0xffffffffULL);
        }

        static uint64_t decode_generation(uint64_t token)
        {
            return token >> 32;
        }
    }

    const int EpollPoller::MAX_EVENT = 4096;

    EpollPoller::EpollPoller()
    {
        data_ = std::make_unique<EpollPoller::HelperData>();
        //signal(SIGPIPE, SIG_IGN);
        data_->epoll_fd_ = ::epoll_create(65535);
        // 设置 ET 触发模式 ？
        data_->epoll_events_.resize(10);
    }

    EpollPoller::~EpollPoller()
    {
    }

    bool EpollPoller::init()
    {
        return data_->epoll_fd_ != -1;
    }

    uint64_t EpollPoller::poll(uint32_t timeout, std::vector<PollEvent> & events)
    {
        int nevent = -1;
        do {
            nevent = ::epoll_wait(data_->epoll_fd_, &*data_->epoll_events_.begin(), (int)data_->epoll_events_.size(), timeout);
        } while (nevent < 0 && app::ClassifyNativeError(app::GetLastNativeError()) == app::NativeError::interrupted);

        uint64_t tm = base::time::get_tick_count();
        if (nevent < 0) {
            return tm;
        }

        if (nevent > 0) {
            for (int i = 0; i < nevent; ++i) {
                int ev = Channel::NONE_EVENT;
                int event = data_->epoll_events_[i].events;
                const uint64_t token = data_->epoll_events_[i].data.u64;
                const int fd = decode_fd(token);
                auto channel_it = data_->channels_.find(fd);
                Channel *channel = channel_it != data_->channels_.end() ? channel_it->second : nullptr;

                if (event & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                    ev |= channel ? (channel->get_events() & (Channel::READ_EVENT | Channel::WRITE_EVENT)) : Channel::READ_EVENT;
                    if (ev == Channel::NONE_EVENT) {
                        ev = Channel::READ_EVENT;
                    }
                } else if (event & EPOLLIN) {
                    ev |= Channel::READ_EVENT;
                }

                if (event & EPOLLOUT) {
                    ev |= Channel::WRITE_EVENT;
                }

                if (ev != Channel::NONE_EVENT) {
                    PollEvent pe;
                    pe.fd = fd;
                    pe.revents = ev;
                    pe.generation = decode_generation(token);
                    events.push_back(pe);
                }
            }

            if (nevent == (int)data_->epoll_events_.size() && (int)data_->epoll_events_.size() < MAX_EVENT) {
                data_->epoll_events_.resize(data_->epoll_events_.size() * 2 >= MAX_EVENT ? MAX_EVENT : data_->epoll_events_.size() * 2);
            }
        }

        return tm;
    }

    void EpollPoller::update_channel(Channel * channel)
    {
        const int fd = channel->get_fd();
        auto it = data_->fds_.find(fd);
        if (it != data_->fds_.end()) {
            if (!channel->has_events()) {
                remove_channel(channel);
            } else {
                data_->channels_[fd] = channel;
                update(EPOLL_CTL_MOD, channel);
            }
        } else {
            data_->channels_[fd] = channel;
            update(EPOLL_CTL_ADD, channel);
            data_->fds_.insert(fd);
        }
    }

    void EpollPoller::remove_channel(Channel * channel)
    {
        update(EPOLL_CTL_DEL, channel);
        data_->fds_.erase(channel->get_fd());
        data_->channels_.erase(channel->get_fd());
    }

    void EpollPoller::update(int op, Channel * channel)
    {
        int ret = 0;
        if (op != EPOLL_CTL_DEL) {
            struct epoll_event event;
            memset(&event, 0, sizeof(struct epoll_event));

            event.events |= EPOLLET | EPOLLRDHUP;
            int ev = channel->get_events();
            if (ev & Channel::READ_EVENT) {
                event.events |= EPOLLIN | EPOLLERR | EPOLLHUP;
            }

            if (ev & Channel::WRITE_EVENT) {
                event.events |= EPOLLOUT;
            }

            event.data.u64 = encode_token(channel->get_fd(), channel->generation());

            ret = ::epoll_ctl(data_->epoll_fd_, op, channel->get_fd(), &event);
        } else {
            ret = ::epoll_ctl(data_->epoll_fd_, op, channel->get_fd(), NULL);
        }

        if (ret == -1) {
            // log
        }
    }
}
#endif
