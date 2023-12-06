#ifndef __POLL_POLLER_H__
#define __POLL_POLLER_H__

#include "net/poller/poller.h"
namespace net
{
    class PollPoller : public Poller
    {
    public:
        PollPoller(EventLoop *loop);

        virtual time_t poll(int timeout, std::vector<Channel *> *activeChannels);

        virtual void update_channel(Channel *channel);

        virtual void remove_channel(Channel *channel);
    private:
        
    };
}

#endif
