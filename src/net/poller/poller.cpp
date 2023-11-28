#include "net/poller/poller.h"

namespace net 
{
    Poller::Poller(EventLoop *loop) : ev_loop_(loop) {}
}