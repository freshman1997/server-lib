#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <signal.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <set>

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
    
    KQueuePoller::KQueuePoller() : data_(std::make_unique<KQueuePoller::HelperData>())
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

    uint64_t KQueuePoller::poll(uint32_t timeout, std::vector<Channel *> &channels)
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
            for (int i = 0; i < count; ++i) {
                int ev = Channel::NONE_EVENT;
                const auto &event = data_->kqueue_events_[i];
                if (event.filter == EVFILT_READ) {
                    ev |= Channel::READ_EVENT;
                }

                if (event.filter == EVFILT_WRITE) {
                    ev |= Channel::WRITE_EVENT;
                }

                Channel *channel = static_cast<Channel *>(event.udata);
                if (ev != Channel::NONE_EVENT && channel) {
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

    void KQueuePoller::update_channel(Channel *channel)
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

    void KQueuePoller::remove_channel(Channel *channel)
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
