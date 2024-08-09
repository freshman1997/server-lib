#ifdef __APPLE__
#include <signal.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <set>

#include "base/time.h"
#include "net/base/channel/channel.h"
#include "net/base/poller/kqueue_poller.h"

namespace net
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

    }

    uint64_t KQueuePoller::poll(uint32_t timeout)
    {
        uint64_t tm = base::time::get_tick_count();
        timespec time;
        time.tv_sec = 0;
        time.tv_nsec = timeout * 1000 * 1000;
        int count = kevent(data_->kqueuefd_, NULL, 0, &*data_->kqueue_events_.begin(), data_->kqueue_events_.size(), &time);
        if (count > 0) {
            for (int i = 0; i < count; ++i) {
                int ev = Channel::NONE_EVENT;
                if (data_->kqueue_events_[i].filter == EVFILT_READ) {
                    ev |= Channel::READ_EVENT;
                }

                if (data_->kqueue_events_[i].filter == EVFILT_WRITE) {
                    ev |= Channel::WRITE_EVENT;
                }

                if (ev != Channel::NONE_EVENT) {
                    Channel *channel = static_cast<Channel *>(data_->kqueue_events_[i].udata);
                    channel->on_event(ev);
                }
            }

            if (count == (int)data_->kqueue_events_.size() && (int)data_->kqueue_events_.size() < MAX_EVENT) {
                data_->kqueue_events_.resize(data_->kqueue_events_.size() * 2 >= MAX_EVENT ? MAX_EVENT : data_->kqueue_events_.size() * 2);
            }
        } else {
            // log
        }

        return tm;
    }

    void KQueuePoller::update_channel(Channel *channel)
    {
        struct kevent event;
        EV_SET(&event, channel->get_fd(), EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, (void *) channel);
        kevent(data_->kqueuefd_, &event, 1, NULL, 0, NULL);
        EV_SET(&event, channel->get_fd(), EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, (void *) channel);
        kevent(data_->kqueuefd_, &event, 1, NULL, 0, NULL);
    }

    void KQueuePoller::remove_channel(Channel *channel)
    {
        struct kevent event;
        EV_SET(&event, channel->get_fd(), EVFILT_READ, EV_DELETE, 0, 0, (void *) channel);
        kevent(data_->kqueuefd_, &event, 1, NULL, 0, NULL);
        EV_SET(&event, channel->get_fd(), EVFILT_WRITE, EV_DELETE, 0, 0, (void *) channel);
        kevent(data_->kqueuefd_, &event, 1, NULL, 0, NULL);
    }
}

#endif