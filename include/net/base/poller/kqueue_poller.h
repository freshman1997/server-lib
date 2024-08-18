#ifndef __NET_BASE_POLLER_KQUEUE_POLLER_H__
#define __NET_BASE_POLLER_KQUEUE_POLLER_H__
#include "../poller/poller.h"
#include <memory>
namespace net
{
    class KQueuePoller : public Poller
    {
        static const int MAX_EVENT;

    public:
        KQueuePoller();

        virtual uint64_t poll(uint32_t timeout, std::vector<Channel *> &channels);

        virtual void update_channel(Channel *channel);

        virtual void remove_channel(Channel *channel);
        
    private:
        class HelperData;
        std::unique_ptr<HelperData> data_;
    };
}

#endif
