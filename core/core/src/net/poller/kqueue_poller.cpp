#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <signal.h>
#include <algorithm>
#include <unistd.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <set>
#include <unordered_map>

#include "base/time.h"
#include "net/channel/channel.h"
#include "net/poller/kqueue_poller.h"

#include <cstdint>

namespace yuan::net
{
    class KQueuePoller::HelperData
    {
    public:
        int kqueuefd_;
        std::set<int> fds_;
        std::unordered_map<int, Channel *> channels_;
        std::vector<struct kevent> kqueue_events_;
        std::vector<PollEvent> merged_events_;
    };

    namespace
    {
        static uintptr_t encode_kqueue_token(int fd, uint64_t generation)
        {
            return (static_cast<uintptr_t>(generation) << 32) | static_cast<uint32_t>(fd);
        }

        static int decode_kqueue_fd(uintptr_t token)
        {
            return static_cast<int>(token & 0xffffffffULL);
        }

        static uint64_t decode_kqueue_generation(uintptr_t token)
        {
            return static_cast<uint64_t>(token >> 32);
        }
    }

    const int KQueuePoller::MAX_EVENT = 4096;

    KQueuePoller::KQueuePoller()
        : data_(std::make_unique<KQueuePoller::HelperData>())
    {
        signal(SIGPIPE, SIG_IGN);
        data_->kqueuefd_ = ::kqueue();
        data_->kqueue_events_.resize(10);
    }

    KQueuePoller::~KQueuePoller()
    {
        if (data_->kqueuefd_ != -1) {
            close(data_->kqueuefd_);
        }
    }

    bool KQueuePoller::init()
    {
        return data_->kqueuefd_ != -1;
    }

    uint64_t KQueuePoller::poll(uint32_t timeout, std::vector<PollEvent> & events)
    {
        timespec time;
        time.tv_sec = timeout / 1000;
        time.tv_nsec = static_cast<long>((timeout % 1000) * 1000 * 1000);

        int count = kevent(data_->kqueuefd_,
                           nullptr,
                           0,
                           data_->kqueue_events_.data(),
                           static_cast<int>(data_->kqueue_events_.size()),
                           &time);
        uint64_t tm = base::time::get_tick_count();
        if (count < 0) {
            return tm;
        }

        if (count > 0) {
            // Kqueue 返回的事件粒度是“每个 filter 一条 kevent”。
            // 如果同一个 fd 同时就绪读/写，就会出现同一个 Channel* 被 push 多次。
            // 但 EventLoop 遍历时会反复调用 channel->on_event()，而 channel->revent_ 会在
            // poll 循环内被后一次 set_revent 覆盖，导致前一次回调丢失。
            // 因此这里按 Channel 聚合 revent，只 push 一次。
            auto &merged_events = data_->merged_events_;
            merged_events.clear();
            if (merged_events.capacity() < static_cast<size_t>(count)) {
                merged_events.reserve(static_cast<size_t>(count));
            }

            for (int i = 0; i < count; ++i) {
                int ev = Channel::NONE_EVENT;
                const auto &event = data_->kqueue_events_[i];
                const uintptr_t token = reinterpret_cast<uintptr_t>(event.udata);
                const int fd = decode_kqueue_fd(token);
                auto channel_it = data_->channels_.find(fd);
                Channel *channel = channel_it != data_->channels_.end() ? channel_it->second : nullptr;

                // kqueue 对 TCP 关闭/错误会在 flags 里给出 EV_EOF/EV_ERROR。
                // 这里按 epoll 的策略：把“异常/挂断”当作 READ 事件交给连接层处理（read() 会返回 0 / errno）。
                if (event.flags & (EV_EOF | EV_ERROR)) {
                    ev |= channel ? (channel->get_events() & (Channel::READ_EVENT | Channel::WRITE_EVENT)) : Channel::READ_EVENT;
                    if (ev == Channel::NONE_EVENT) {
                        ev = Channel::READ_EVENT;
                    }
                }

                if (event.filter == EVFILT_READ) {
                    ev |= Channel::READ_EVENT;
                }

                if (event.filter == EVFILT_WRITE) {
                    ev |= Channel::WRITE_EVENT;
                }

                if (ev != Channel::NONE_EVENT) {
                    auto found = std::find_if(merged_events.begin(), merged_events.end(), [fd](const PollEvent &item) {
                        return item.fd == fd;
                    });
                    if (found == merged_events.end()) {
                        merged_events.push_back(PollEvent{ fd, ev, decode_kqueue_generation(token) });
                        continue;
                    }
                    auto &pe = *found;
                    pe.fd = fd;
                    pe.generation = decode_kqueue_generation(token);
                    pe.revents |= ev;
                }
            }

            for (auto &pe : merged_events) {
                if (pe.revents != Channel::NONE_EVENT) {
                    events.push_back(pe);
                }
            }

            if (count == static_cast<int>(data_->kqueue_events_.size()) && static_cast<int>(data_->kqueue_events_.size()) < MAX_EVENT) {
                const auto next_size = static_cast<int>(data_->kqueue_events_.size()) * 2;
                data_->kqueue_events_.resize(next_size >= MAX_EVENT ? MAX_EVENT : next_size);
            }
        }

        return tm;
    }

    void KQueuePoller::update_channel(Channel * channel)
    {
        auto it = data_->fds_.find(channel->get_fd());
        if (!channel->has_events()) {
            if (it != data_->fds_.end()) {
                remove_channel(channel);
            }
            return;
        }

        data_->channels_[channel->get_fd()] = channel;
        const uintptr_t token = encode_kqueue_token(channel->get_fd(), channel->generation());

        struct kevent events[2];
        int count = 0;

        if (channel->get_events() & Channel::READ_EVENT) {
            EV_SET(&events[count++], channel->get_fd(), EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, reinterpret_cast<void*>(token));
        } else if (it != data_->fds_.end()) {
            EV_SET(&events[count++], channel->get_fd(), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        }

        if (channel->get_events() & Channel::WRITE_EVENT) {
            EV_SET(&events[count++], channel->get_fd(), EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, reinterpret_cast<void*>(token));
        } else if (it != data_->fds_.end()) {
            EV_SET(&events[count++], channel->get_fd(), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        }

        if (count > 0) {
            kevent(data_->kqueuefd_, events, count, nullptr, 0, nullptr);
        }

        data_->fds_.insert(channel->get_fd());
    }

    void KQueuePoller::remove_channel(Channel * channel)
    {
        auto it = data_->fds_.find(channel->get_fd());
        if (it == data_->fds_.end()) {
            return;
        }

        data_->fds_.erase(it);
        data_->channels_.erase(channel->get_fd());

        struct kevent events[2];
        EV_SET(&events[0], channel->get_fd(), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        EV_SET(&events[1], channel->get_fd(), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        kevent(data_->kqueuefd_, events, 2, nullptr, 0, nullptr);
    }
}

#endif
