#ifndef __NET_SECURITY_SSL_HANDLER_H__
#define __NET_SECURITY_SSL_HANDLER_H__
#include <cstddef>
#include <string_view>

namespace yuan::net
{
    class SSLHandler
    {
    public:
        enum class SSLMode {
            acceptor_,
            connector_
        };

    public:
        virtual ~SSLHandler()
        {
        }

        virtual int ssl_init_action() = 0;

        virtual int ssl_write(const char *data, std::size_t size) = 0;

        virtual int ssl_read(char *buffer, std::size_t size) = 0;

        virtual bool ssl_want_read() const
        {
            return false;
        }

        virtual bool ssl_want_write() const
        {
            return false;
        }

        virtual std::string_view get_alpn_selected() const
        {
            return {};
        }
    };
}

#endif
