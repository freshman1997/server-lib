#ifndef __SOCKET_H__
#define __SOCKET_H__
#include "inet_address.h"

namespace net
{
    class Socket
    {
    public:
        explicit Socket(const char *ip, int port);
        ~Socket();

        bool bind();

        bool listen();

        int accept(struct sockaddr_in &peer_addr);

        bool connect();

        void set_no_deylay(bool on);

        void set_reuse(bool on);

        void set_keep_alive(bool on);

        void set_none_block(bool on);

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
        InetAddress *addr;
        const int fd_;
    };
}
#endif
