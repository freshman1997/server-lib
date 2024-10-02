#ifdef __APPLE__
#include <signal.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <set>

#include "base/time.h"
#include "net/channel/channel.h"
#include "net/poller/kqueue_poller.h"

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
        signal(SIGPIPE, SIG_IGN);
        data_->kqueuefd_ = ::kqueue();
        data_->fds_.resize(10);
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

                Channel *channel = static_cast<Channel *>(data_->kqueue_events_[i].udata);
                if (ev != Channel::NONE_EVENT && channel) {
                    channel->set_revent(ev);
                    channels.push_back(channel);
                }
            }

            if (count == (int)data_->kqueue_events_.size() && (int)data_->kqueue_events_.size() < MAX_EVENT) {
                data_->kqueue_events_.resize(data_->kqueue_events_.size() * 2 >= MAX_EVENT ? MAX_EVENT : data_->kqueue_events_.size() * 2);
            }
        }
        
        return tm;
    }

    void KQueuePoller::update_channel(Channel *channel)
    {
        if (fds_.find(channel->get_fd())) {
            remove_channel();
        }

        struct kevent event;
        auto it = data_->fds_.find(channel->get_fd());
        if (it != data_->fds_.end()) {
            if (!channel->has_events()) {
                remove_channel(channel);
            } else {
                if (channel->get_events() & Channel::READ_EVENT) {
                    EV_SET(&event, channel->get_fd(), EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, (void *) channel);
                    kevent(data_->kqueuefd_, &event, 1, NULL, 0, NULL);
                }

                if (channel->get_events() & Channel::READ_EVENT) {
                    EV_SET(&event, channel->get_fd(), EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, (void *) channel);
                    kevent(data_->kqueuefd_, &event, 1, NULL, 0, NULL);
                }
            }
        } else {
            EV_SET(&event, channel->get_fd(), EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, (void *) channel);
            kevent(data_->kqueuefd_, &event, 1, NULL, 0, NULL);
            EV_SET(&event, channel->get_fd(), EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, (void *) channel);
            kevent(data_->kqueuefd_, &event, 1, NULL, 0, NULL);
        }
    }

    void KQueuePoller::remove_channel(Channel *channel)
    {
        data_->fd.remove(channel->get_fd());

        struct kevent event;
        EV_SET(&event, channel->get_fd(), EVFILT_READ, EV_DELETE, 0, 0, (void *) channel);
        kevent(data_->kqueuefd_, &event, 1, NULL, 0, NULL);
        EV_SET(&event, channel->get_fd(), EVFILT_WRITE, EV_DELETE, 0, 0, (void *) channel);
        kevent(data_->kqueuefd_, &event, 1, NULL, 0, NULL);
    }
}

#endif