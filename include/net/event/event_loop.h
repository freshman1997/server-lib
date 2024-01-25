#ifndef __EVENT_LOOH_H__
#define __EVENT_LOOH_H__
#include <unordered_map>

#include "net/handler/accept_handler.h"
#include "net/handler/connection_handler.h"

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
        EventLoop(Poller *_poller, timer::TimerManager *timer_manager, Acceptor *acceptor);
        ~EventLoop();

    public:
        void loop();

        void quit()
        {
            quit_ = true;
        }

        virtual void on_new_connection(Connection *conn, Acceptor *acceptor);

        virtual void on_quit(Acceptor *acceptor);

        virtual void on_close(Connection *conn);

        virtual bool is_unique(int fd);

    public:
        void set_tcp_handler(ConnectionHandler *connHandler)
        {
            this->connHandler_ = connHandler;
        }

    private:
        bool quit_;
        Poller *poller_;
        timer::TimerManager *timer_manager_;
        std::unordered_map<int, Channel *> channels_;
        ConnectionHandler *connHandler_;
    };
}
#endif
