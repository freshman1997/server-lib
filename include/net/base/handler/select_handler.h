#ifndef __SELECT_HANDLER_H__
#define __SELECT_HANDLER_H__

namespace net
{
    class EventHandler;

    class SelectHandler
    {
    public:
        virtual ~SelectHandler() {}

        virtual void on_read_event() = 0;

        virtual void on_write_event() = 0;

        virtual void set_event_handler(EventHandler *eventHandler) = 0;
    };
}

#endif
