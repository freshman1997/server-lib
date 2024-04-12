#ifndef __SELECT_POLLER_H__
#define __SELECT_POLLER_H__
#include "../poller/poller.h"
namespace net 
{
    class SelectPoller : public Poller
    {
    public:
        SelectPoller();

        virtual uint64_t poll(uint32_t timeout);

        virtual void update_channel(Channel *channel);

        virtual void remove_channel(Channel *channel);
    };
}

#endif