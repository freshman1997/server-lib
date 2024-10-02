#ifndef __NET_BASE_POLLER_SELECT_POLLER_H__
#define __NET_BASE_POLLER_SELECT_POLLER_H__
#include "poller.h"
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <memory>

namespace net 
{
    class SelectPoller : public Poller
    {
    public:
        SelectPoller();
        
        ~SelectPoller();

        virtual bool init();

        virtual uint64_t poll(uint32_t timeout, std::vector<Channel *> &channels);

        virtual void update_channel(Channel *channel);

        virtual void remove_channel(Channel *channel);

    private:
        class HelperData;
        std::unique_ptr<HelperData> data_;
    };
}

#endif