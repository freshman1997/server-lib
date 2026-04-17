#ifndef __NET_SOCKS5_SOCKS5_HANDLER_H__
#define __NET_SOCKS5_SOCKS5_HANDLER_H__

#include <cstdint>
#include <string>

namespace yuan::net::socks5
{
    class Socks5Session;

    class Socks5Handler
    {
    public:
        virtual ~Socks5Handler() = default;

        virtual bool on_authenticate(const std::string &username, const std::string &password) = 0;

        virtual bool on_connect_request(Socks5Session *session, const std::string &host, uint16_t port) = 0;

        virtual void on_session_opened(Socks5Session *session) = 0;

        virtual void on_session_closed(Socks5Session *session) = 0;
    };
}

#endif
