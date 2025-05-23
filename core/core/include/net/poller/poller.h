#ifndef __POLLER_H__
#define __POLLER_H__
#include <cstdint>
#include <vector>

namespace yuan::net 
{
    class Channel;
    class Poller
    {
    public:
        virtual ~Poller() {}

        virtual bool init() = 0;
        
        virtual uint64_t poll(uint32_t timeout, std::vector<Channel *> &channels) = 0;

        virtual void update_channel(Channel *channel) = 0;

        virtual void remove_channel(Channel *channel) = 0;

    };
}
#endif
