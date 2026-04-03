#ifndef __NET_SECUITY_SSL_HANDLER_H__
#define __NET_SECUITY_SSL_HANDLER_H__
#include "buffer/buffer.h"

namespace yuan::buffer
{
    class Buffer;
}

namespace yuan::net
{
    namespace buffer { using ::yuan::buffer::Buffer; }
}

namespace yuan::net
{
    class SSLHandler
    {
    public:
        enum class SSLMode
        {
            acceptor_,
            connector_
        };

    public:
        virtual ~SSLHandler() {}

        virtual int ssl_init_action() = 0;

        virtual int ssl_write(buffer::Buffer *buff) = 0;

        virtual int ssl_read(buffer::Buffer *buff) = 0;
    };
}

#endif
