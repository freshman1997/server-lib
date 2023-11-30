#ifndef __EPOLL_POLLER_H__
#define __EPOLL_POLLER_H__
#include <vector>
#include "poller.h"

namespace net {
    class EpollPoller : public Poller
    {
    public:
        EpollPoller(EventLoop *loop);
        ~EpollPoller();

        virtual time_t poll(int timeout, std::vector<Channel *> *activeChannels);

        virtual void update_channel(Channel *channel);

        virtual void remove_channel(Channel *channel);

    private:
        void update(int op, Channel *);

    private:
        int epoll_fd_;
        std::vector<struct epoll_event> epoll_events_;
    };
}
#endif