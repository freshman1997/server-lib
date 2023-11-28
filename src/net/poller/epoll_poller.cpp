#include "net/poller/epoll_poller.h"
#include "net/poller/poller.h"

namespace net 
{

    EpollPoller::EpollPoller(EventLoop *loop) : Poller(loop) {}

    time_t EpollPoller::poll(int timeout, std::vector<Channel *> *activeChannels)
    {
        return 0;
    }

    void EpollPoller::update_channel(Channel *channel)
    {

    }

    void EpollPoller::remove_channel(Channel *channel)
    {

    }
}