#ifndef __NET_BASE_POLLER_SELECT_POLLER_H__
#define __NET_BASE_POLLER_SELECT_POLLER_H__
#include "../poller/poller.h"
namespace net 
{
    struct HelperData;

    class SelectPoller : public Poller
    {
    public:
        SelectPoller();
        
        ~SelectPoller();

        virtual uint64_t poll(uint32_t timeout);

        virtual void update_channel(Channel *channel);

        virtual void remove_channel(Channel *channel);
    private:
        HelperData *data_;
    };
}

#endif