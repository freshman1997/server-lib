#ifndef __POLLER_H__
#define __POLLER_H__
#include <ctime>
namespace net 
{
    class Channel;
    class Poller
    {
    public:
        virtual ~Poller() {}
        
        virtual time_t poll(int timeout) = 0;

        virtual void update_channel(Channel *channel) = 0;

        virtual void remove_channel(Channel *channel) = 0;

    };
}
#endif
