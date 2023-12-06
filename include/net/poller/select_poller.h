#ifndef __SELECT_POLLER_H__
#define __SELECT_POLLER_H__

#include "net/poller/poller.h"
namespace net 
{
    class SelectPoller : public Poller
    {
    public:
        SelectPoller(EventLoop *loop);

        virtual time_t poll(int timeout, std::vector<Channel *> *activeChannels);

        virtual void update_channel(Channel *channel);

        virtual void remove_channel(Channel *channel);
    
    private:
        
    };
}

#endif