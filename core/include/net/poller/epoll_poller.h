#ifndef __NET_BASE_POLLER_EPOLL_POLLER_H__
#define __NET_BASE_POLLER_EPOLL_POLLER_H__
#include <memory>
#include "poller.h"

namespace yuan::net 
{
    class EpollPoller : public Poller
    {
        static const int MAX_EVENT;

    public:
        EpollPoller();
        ~EpollPoller();

        virtual bool init();

        virtual uint64_t poll(uint32_t timeout, std::vector<Channel *> &channels);

        virtual void update_channel(Channel *channel);

        virtual void remove_channel(Channel *channel);

    private:
        void update(int op, Channel *);

    private:
        class HelperData;
        std::unique_ptr<HelperData> data_;
    };
}
#endif
