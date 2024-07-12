#ifndef __EVENT_HANDLER_H__
#define __EVENT_HANDLER_H__

namespace net
{
    class Connection;
    class Acceptor;
    class Channel;

    class EventHandler
    {
    public:
        virtual void on_new_connection(Connection *conn) = 0;

        virtual void on_quit() = 0;

        virtual void on_close(Connection *conn) = 0;

        virtual void update_event(Channel *channel) = 0;
    };
}

#endif