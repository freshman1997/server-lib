#ifndef __EVENT_HANDLER_H__
#define __EVENT_HANDLER_H__

#include <functional>

namespace yuan::net
{
    class Connection;
    class Acceptor;
    class Channel;

    class EventHandler
    {
    public:
        virtual void on_new_connection(Connection *conn) = 0;

        virtual void close_channel(Channel *channel) = 0;

        virtual void update_channel(Channel *channel) = 0;

        virtual void quit() = 0;

        virtual void queue_in_loop(std::function<void()> cb) = 0;
    };
}

#endif