#ifndef __EPOLL_POLLER_H__
#define __EPOLL_POLLER_H__
#include "poller.h"

namespace net {
class EpollPoller : public Poller
{
public:
    EpollPoller(EventLoop *loop);

    virtual time_t poll(int timeout, std::vector<Channel *> *activeChannels);

    virtual void update_channel(Channel *channel);

    virtual void remove_channel(Channel *channel);
};

}
#endif