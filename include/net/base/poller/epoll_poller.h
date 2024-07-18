#ifndef __NET_BASE_POLLER_EPOLL_POLLER_H__
#define __NET_BASE_POLLER_EPOLL_POLLER_H__
#include <set>
#include <vector>
#include "poller.h"

namespace net {
    class EpollPoller : public Poller
    {
        static const int MAX_EVENT;

    public:
        EpollPoller();
        ~EpollPoller();

        virtual uint64_t poll(uint32_t timeout);

        virtual void update_channel(Channel *channel);

        virtual void remove_channel(Channel *channel);

    private:
        void update(int op, Channel *);

    private:
        int epoll_fd_;
        std::set<int> fds_;
        std::vector<struct epoll_event> epoll_events_;
    };
}
#endif
