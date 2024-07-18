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

        virtual void on_new_connection(Connection *conn);

        virtual void close_channel(Channel *channel);

        virtual void update_channel(Channel *channel);

        virtual void quit();

    public:
        void wakeup();

    private:
        bool quit_;
        bool is_waiting_;
        Poller *poller_;
        timer::TimerManager *timer_manager_;
        std::unordered_map<int, Channel *> channels_;
    };
}
#endif
