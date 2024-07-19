#ifndef __NET_BASE_POLLER_SELECT_POLLER_H__
#define __NET_BASE_POLLER_SELECT_POLLER_H__
#include "../poller/poller.h"
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <map>
#include <vector>

namespace net 
{
    class SelectPoller : public Poller
    {
    public:
        SelectPoller();
        
        ~SelectPoller();

        virtual uint64_t poll(uint32_t timeout);

        virtual void update_channel(Channel *channel);

        virtual void remove_channel(Channel *channel);

    private:
        fd_set reads_;
        fd_set writes_;
		fd_set excepts_;
        std::map<int, net::Channel *> sockets_;
        std::vector<int> removed_fds_;
    };
}

#endif