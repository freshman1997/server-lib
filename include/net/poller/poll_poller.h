#ifndef __NET_BASE_POLLER_POLL_POLLER_H__
#define __NET_BASE_POLLER_POLL_POLLER_H__
#include "poller.h"
#include <memory>
namespace net
{
    struct HelperData;

    class PollPoller : public Poller
    {
    public:
        PollPoller();

        ~PollPoller();

        virtual bool init();

        virtual uint64_t poll(uint32_t timeout, std::vector<Channel *> &channels);

        virtual void update_channel(Channel *channel);

        virtual void remove_channel(Channel *channel);
    private:
        void do_add_channel(Channel *channel);

    private:
        class HelperData;
        std::unique_ptr<HelperData> data_;
    };
}

#endif
