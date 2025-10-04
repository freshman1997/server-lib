#ifndef __SOCKET_H__
#define __SOCKET_H__
#include "net/secuity/ssl_handler.h"
#include <memory>

namespace yuan::net
{
    class InetAddress;
    
    class Socket
    {
    public:
        explicit Socket(const char *ip, int port, bool udp = false, int fd = -1);
        ~Socket();

        bool bind() const;

        bool listen() const;

        int accept(struct sockaddr_in &peer_addr) const;

        bool connect(const std::shared_ptr<SSLHandler> &sslModule = nullptr) const;

        void set_no_delay(bool on) const;

        bool set_reuse(bool on, bool exclude = false) const;

        void set_keep_alive(bool on) const;

        void set_none_block(bool on) const;

        int get_fd() const
        {
            return fd_;
        }

        bool valid()
        {
            return fd_ > 0;
        }

        InetAddress * get_address() 
        {
            return addr;
        }

    private:
        int fd_;
        InetAddress *addr;
    };
}
#endif
