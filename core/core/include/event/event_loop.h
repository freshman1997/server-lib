#ifndef __EVENT_LOOH_H__
#define __EVENT_LOOH_H__
#include <memory>

#include "net/handler/event_handler.h"
#include "net/handler/connection_handler.h"

namespace yuan::timer
{
    class TimerManager;
}

namespace yuan::net 
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
        class HelperData;
        std::unique_ptr<HelperData> data_;
    };
}
#endif
