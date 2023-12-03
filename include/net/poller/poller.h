#ifndef __POLLER_H__
#define __POLLER_H__
#include <ctime>
#include <unordered_map>
#include <vector>
namespace net 
{
    class Channel;
    class EventLoop;
    class Poller
    {
    public:
        Poller(EventLoop *loop);

        virtual time_t poll(int timeout, std::vector<Channel *> *activeChannels) = 0;

        virtual void update_channel(Channel *channel) = 0;

        virtual void remove_channel(Channel *channel) = 0;

    protected:
        EventLoop *ev_loop_;
        std::unordered_map<int, Channel *> channels_;
    };
}
#endif
