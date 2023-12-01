#ifndef __SOCKET_H__
#define __SOCKET_H__
#include "inet_address.h"
#include <string>

namespace net
{
    class Socket
    {
    public:
        explicit Socket(int fd) : fd_(fd) {}
        ~Socket();

        bool bind(const InetAddress &addr);

        bool listen();

        int accept(const InetAddress &addr);

        bool connect(const InetAddress &addr);

        void set_no_deylay(bool on);

        void set_reuse(bool on);

        void set_keep_alive(bool on);

        int get_fd() const
        {
            return fd_;
        }

    private:
        const int fd_;
    };
}
#endif
