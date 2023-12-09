#ifndef __HANDLER_H__
#define __HANDLER_H__

#include "buff/buffer.h"
namespace net
{
    class Handler
    {   
    public:
        virtual void on_packet(Buffer &buff) = 0;

        virtual void on_error() = 0;
    };
}

#endif