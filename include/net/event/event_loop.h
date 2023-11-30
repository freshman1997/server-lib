#ifndef __EVENT_LOOH_H__
#define __EVENT_LOOH_H__

namespace net 
{

class Poller;

class EventLoop 
{
public:
    EventLoop(Poller *_poller);

public:
    void loop();

private:
    Poller *poller_;
};

}
#endif
