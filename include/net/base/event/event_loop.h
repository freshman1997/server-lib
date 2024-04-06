#ifndef __EVENT_LOOH_H__
#define __EVENT_LOOH_H__
#include <unordered_map>

#include "../handler/event_handler.h"
#include "../handler/connection_handler.h"

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

    class EventLoop : public EventHandler
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

        virtual void on_new_connection(Connection *conn, bool callConnected = true);

        virtual void on_quit();

        virtual void on_close(Connection *conn);

        virtual bool is_unique(int fd);

        virtual void update_event(Channel *channel);

    public:
        void set_connection_handler(ConnectionHandler *connHandler)
        {
            this->connHandler_ = connHandler;
        }

        void wakeup();

    private:
        bool quit_;
        bool is_waiting_;
        Poller *poller_;
        timer::TimerManager *timer_manager_;
        std::unordered_map<int, Channel *> channels_;
        ConnectionHandler *connHandler_;
    };
}
#endif
