#ifndef __EVENT_LOOH_H__
#define __EVENT_LOOH_H__

#include "net/acceptor/acceptor.h"
#include "net/connection/connection.h"
#include "net/socket/socket.h"
#include <unordered_map>
namespace net 
{

class Poller;

class EventLoop 
{
public:
    EventLoop(Poller *_poller);
    ~EventLoop();

public:
    void loop();

    void quit()
    {
        quit_ = true;
    }

    void start();

private:
    bool quit_;
    net::Socket *serverSocket_;
    Acceptor *acceptor_;
    Poller *poller_;
    std::unordered_map<int, net::Connection *> connections;
};

}
#endif
