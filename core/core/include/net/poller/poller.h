#ifndef __POLLER_H__
#define __POLLER_H__
#include <cstdint>
#include <vector>

namespace yuan::net 
{
    class Channel;

    struct PollEvent
    {
        int fd = -1;
        int revents = 0;
        uint64_t generation = 0;
    };

    class Poller
    {
    public:
        virtual ~Poller() {}

        virtual bool init() = 0;
        
        virtual uint64_t poll(uint32_t timeout, std::vector<PollEvent> &events) = 0;

        virtual void update_channel(Channel *channel) = 0;

        virtual void remove_channel(Channel *channel) = 0;

    };
}
#endif
