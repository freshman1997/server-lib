#ifndef __CONNECTION_HANDLER_H__
#define __CONNECTION_HANDLER_H__
class Buffer;

namespace net
{
    class Handler
    {   
    public:
        virtual void on_packet(Buffer *buff) = 0;

        virtual void on_error() = 0;
    };
}

#endif