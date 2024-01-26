#ifndef __ACCEPTOR_H__
#define __ACCEPTOR_H__
#include "net/handler/select_handler.h"

namespace net
{
    class EventHandler;
    class Connection;
    class Channel;

    class Acceptor : public SelectHandler
    {
    public:
        virtual bool listen() = 0;

        virtual void on_close() = 0;

        virtual Channel * get_channel() = 0;
    };
}

#endif
