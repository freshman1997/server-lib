#ifndef __NET_SECUITY_SSL_HANDLER_H__
#define __NET_SECUITY_SSL_HANDLER_H__
#include <cstddef>

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

        virtual int ssl_write(const char *data, std::size_t size) = 0;

        virtual int ssl_read(char *buffer, std::size_t size) = 0;
    };
}

#endif
