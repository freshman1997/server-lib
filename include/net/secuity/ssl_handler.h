#ifndef __NET_SECUITY_SSL_HANDLER_H__
#define __NET_SECUITY_SSL_HANDLER_H__
#include "buffer/buffer.h"

namespace net 
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

        virtual void set_user_data(void *udata1, void *udata2, SSLMode mode) = 0;

        virtual int ssl_init_action() = 0;

        virtual int ssl_write(Buffer *buff) = 0;

        virtual int ssl_read(Buffer *buff) = 0;
    };
}

#endif
