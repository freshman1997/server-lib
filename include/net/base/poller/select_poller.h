#ifndef __SELECT_POLLER_H__
#define __SELECT_POLLER_H__
#include "../poller/poller.h"
namespace net 
{
    class SelectPoller : public Poller
    {
    public:
        SelectPoller();

        virtual time_t poll(int timeout);

        virtual void update_channel(Channel *channel);

        virtual void remove_channel(Channel *channel);
    };
}

#endif