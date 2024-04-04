#ifndef __POLL_POLLER_H__
#define __POLL_POLLER_H__
#include "../poller/poller.h"
namespace net
{
    class PollPoller : public Poller
    {
    public:
        PollPoller();

        virtual uint32_t poll(uint32_t timeout);

        virtual void update_channel(Channel *channel);

        virtual void remove_channel(Channel *channel);
    };
}

#endif
