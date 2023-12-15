#ifndef __EVENT_LOOH_H__
#define __EVENT_LOOH_H__
#include <unordered_map>

#include "net/handler/accept_handler.h"

namespace timer
{
    class TimerManager;
}

namespace net 
{
    class Poller;
    class Socket;
    class Connection;
    class Channel;

    class EventLoop : public AcceptHandler
    {
    public:
        EventLoop(Poller *_poller, timer::TimerManager *timer_manager);
        ~EventLoop();

    public:
        void loop();

        void quit()
        {
            quit_ = true;
        }

        void start();

        void on_new_connection(Connection *conn, Acceptor *acceptor);

        void on_close(Acceptor *acceptor);

    private:
        bool quit_;
        Poller *poller_;
        timer::TimerManager *timer_manager_;
        std::unordered_map<int, Channel *> channels_;
    };
}
#endif
