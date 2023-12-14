#ifndef __SELECT_HANDLER_H__
#define __SELECT_HANDLER_H__

namespace net
{
    class SelectHandler
    {
    public:
        virtual void on_read_event() = 0;

        virtual void on_write_event() = 0;

        virtual int get_fd() = 0;
    };
}

#endif
