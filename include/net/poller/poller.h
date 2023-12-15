#ifndef __POLLER_H__
#define __POLLER_H__
#include <ctime>
#include <unordered_map>
#include <vector>
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

    protected:
        std::unordered_map<int, Channel *> channels_;
    };
}
#endif
