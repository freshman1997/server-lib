#include "net/event/event_loop.h"

namespace net 
{

EventLoop::EventLoop(Poller *_poller) : poller_(_poller)
{}

void EventLoop::loop()
{

}

}