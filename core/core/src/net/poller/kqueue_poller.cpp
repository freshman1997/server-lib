#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <signal.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <set>
#include <unordered_map>

#include "base/time.h"
#include "net/channel/channel.h"
#include "net/poller/kqueue_poller.h"

namespace yuan::net
{
    class KQueuePoller::HelperData
    {
    public:
        int kqueuefd_;
        std::set<int> fds_;
        std::vector<struct kevent> kqueue_events_;
    };

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

    uint64_t KQueuePoller::poll(uint32_t timeout, std::vector<Channel *> & channels)
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
            std::unordered_map<Channel *, int> revents_by_channel;
            revents_by_channel.reserve(static_cast<size_t>(count));

            for (int i = 0; i < count; ++i) {
                int ev = Channel::NONE_EVENT;
                const auto &event = data_->kqueue_events_[i];

                // kqueue 对 TCP 关闭/错误会在 flags 里给出 EV_EOF/EV_ERROR。
                // 这里按 epoll 的策略：把“异常/挂断”当作 READ 事件交给连接层处理（read() 会返回 0 / errno）。
                if (event.flags & (EV_EOF | EV_ERROR)) {
                    ev |= Channel::READ_EVENT;
                }

                if (event.filter == EVFILT_READ) {
                    ev |= Channel::READ_EVENT;
                }

                if (event.filter == EVFILT_WRITE) {
                    ev |= Channel::WRITE_EVENT;
                }

                Channel *channel = static_cast<Channel *>(event.udata);
                if (ev != Channel::NONE_EVENT && channel) {
                    revents_by_channel[channel] |= ev;
                }
            }

            for (auto & [
                            channel,
                            ev
                        ] : revents_by_channel) {
                if (channel && ev != Channel::NONE_EVENT) {
                    channel->set_revent(ev);
                    channels.push_back(channel);
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

        struct kevent events[2];
        int count = 0;

        if (channel->get_events() & Channel::READ_EVENT) {
            EV_SET(&events[count++], channel->get_fd(), EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, channel);
        } else if (it != data_->fds_.end()) {
            EV_SET(&events[count++], channel->get_fd(), EVFILT_READ, EV_DELETE, 0, 0, channel);
        }

        if (channel->get_events() & Channel::WRITE_EVENT) {
            EV_SET(&events[count++], channel->get_fd(), EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, channel);
        } else if (it != data_->fds_.end()) {
            EV_SET(&events[count++], channel->get_fd(), EVFILT_WRITE, EV_DELETE, 0, 0, channel);
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

        struct kevent events[2];
        EV_SET(&events[0], channel->get_fd(), EVFILT_READ, EV_DELETE, 0, 0, channel);
        EV_SET(&events[1], channel->get_fd(), EVFILT_WRITE, EV_DELETE, 0, 0, channel);
        kevent(data_->kqueuefd_, events, 2, nullptr, 0, nullptr);
    }
}

#endif
