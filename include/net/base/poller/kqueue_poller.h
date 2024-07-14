#ifndef __NET_BASE_POLLER_KQUEUE_POLLER_H__
#define __NET_BASE_POLLER_KQUEUE_POLLER_H__
#include "../poller/poller.h"
namespace net
{
    class KQueuePoller : public Poller
    {
    public:
        KQueuePoller();

        virtual uint64_t poll(uint32_t timeout);

        virtual void update_channel(Channel *channel);

        virtual void remove_channel(Channel *channel);
    };
}

#endif
