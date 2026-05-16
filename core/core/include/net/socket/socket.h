#ifndef __SOCKET_H__
#define __SOCKET_H__
#include "net/security/ssl_handler.h"
#include "net/socket/listen_options.h"
#include <memory>
#include <string_view>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

namespace yuan::net
{
    class InetAddress;

    class Socket
    {
    public:
        explicit Socket(std::string_view ip, int port, bool udp = false, int fd = -1);
        ~Socket();

        Socket(const Socket &) = delete;
        Socket &operator=(const Socket &) = delete;
        Socket(Socket &&) = delete;
        Socket &operator=(Socket &&) = delete;

        bool bind() const;

        bool listen() const;

        bool listen(int backlog) const;

        int accept(struct sockaddr_storage &peer_addr) const;

        bool connect(const std::shared_ptr<SSLHandler> &sslModule = nullptr) const;

        int last_error() const;

        void set_no_delay(bool on) const;

        bool set_reuse(bool on, bool exclude = false) const;

        bool set_reuse_addr(bool on, bool exclusive = false) const;

        bool set_reuse_port(bool on) const;

        bool apply_listen_options(const ListenOptions &options) const;

        void set_keep_alive(bool on) const;

        void set_none_block(bool on) const;

        bool shutdown_write() const;

        int get_fd() const
        {
            return fd_;
        }

        bool valid() const
        {
            return fd_ >= 0;
        }

        InetAddress *get_address() const
        {
            return addr_ ? &*addr_ : nullptr;
        }

        InetAddress get_local_address() const;

    private:
        int fd_;
        std::unique_ptr<InetAddress> addr_;
    };
}
#endif
