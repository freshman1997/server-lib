#ifndef __POLLER_H__
#define __POLLER_H__
#include <cstdint>

namespace net 
{
    class Channel;
    class Poller
    {
    public:
        virtual ~Poller() {}
        
        virtual uint32_t poll(uint32_t timeout) = 0;

        virtual void update_channel(Channel *channel) = 0;

        virtual void remove_channel(Channel *channel) = 0;

    };
}
#endif
