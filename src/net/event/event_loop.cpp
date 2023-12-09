#include <cassert>

#include "net/event/event_loop.h"
#include "net/poller/poller.h"

namespace net 
{

    EventLoop::EventLoop(Poller *_poller) : poller_(_poller), quit_(false), serverSocket_(nullptr)
    {

    }

    EventLoop::~EventLoop()
    {
        assert(serverSocket_ != nullptr);
    }

    void EventLoop::loop()
    {
        assert(poller_);

        while (!quit_) {
            //poller_->poll(100, this->)
        }
    }

    void EventLoop::start()
    {

    }

}