#include "net/security/ssl_handler.h"
#ifdef _WIN32
#include <io.h>
#include <winsock2.h>
#include <windows.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <unistd.h>
#endif

#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "net/socket/socket_ops.h"
#include "native_platform.h"

namespace yuan::net
{
    namespace
    {
        bool is_connect_in_progress_error()
        {
            const auto err = app::GetLastNativeError();
            const auto kind = app::ClassifyNativeError(err);
            return kind == app::NativeError::would_block || kind == app::NativeError::in_progress ||
                   kind == app::NativeError::already || kind == app::NativeError::invalid_argument;
        }

        bool is_connect_success_error()
        {
#ifdef _WIN32
            return app::GetLastNativeError() == WSAEISCONN;
#else
            return app::GetLastNativeError() == EISCONN;
#endif
        }
    } // namespace

    Socket::Socket(std::string_view ip, int port, bool udp, int fd)
    {
        fd_ = fd;

        const std::string input_ip = ip.empty() ? "" : std::string(ip);
        const std::string realIp = input_ip.empty() ? "" : InetAddress::normalize_host(input_ip);
        addr_ = std::make_unique<InetAddress>(realIp.empty() ? input_ip : realIp, port);
        if (fd < 0) {
            if (addr_->is_ipv6()) {
                if (!udp) {
                    fd_ = socket::create_ipv6_tcp_socket(true);
                } else {
                    fd_ = socket::create_ipv6_udp_socket(true);
                }
            } else {
                if (!udp) {
                    fd_ = socket::create_ipv4_tcp_socket(true);
                } else {
                    fd_ = socket::create_ipv4_udp_socket(true);
                }
            }
        }
    }

    Socket::~Socket()
    {
        if (fd_ >= 0) {
            socket::close_fd(fd_);
            fd_ = -1;
        }
    }

    bool Socket::bind() const
    {
        return socket::bind(fd_, *addr_) == 0;
    }

    bool Socket::listen() const
    {
        return listen(128);
    }

    bool Socket::listen(const int backlog) const
    {
        return socket::listen(fd_, backlog) == 0;
    }

    int Socket::accept(struct sockaddr_storage & peer_addr) const
    {
        const int conn_fd = socket::accept(fd_, peer_addr);
        if (conn_fd < 0) {
            // log
        }

        return conn_fd;
    }

    bool Socket::connect(const std::shared_ptr<SSLHandler> & sslModule) const
    {
        int res = -1;
        if (sslModule) {
            res = sslModule->ssl_init_action();
        } else {
            res = socket::connect(fd_, *addr_);
        }
        if (res < 0) {
            if (is_connect_in_progress_error() || is_connect_success_error()) {
                return true;
            }
            return false;
        }

        return res == 0;
    }

    InetAddress Socket::get_local_address() const
    {
        return socket::get_local_address(fd_);
    }

    int Socket::last_error() const
    {
        return socket::get_last_error();
    }

    void Socket::set_no_delay(const bool on) const
    {
        socket::set_no_delay(fd_, on);
    }

    bool Socket::set_reuse(const bool on, const bool exclude) const
    {
        return socket::set_reuse(fd_, on, exclude);
    }

    bool Socket::set_reuse_addr(const bool on, const bool exclusive) const
    {
        return socket::set_reuse_addr(fd_, on, exclusive);
    }

    bool Socket::set_reuse_port(const bool on) const
    {
        return socket::set_reuse_port(fd_, on);
    }

    bool Socket::apply_listen_options(const ListenOptions &options) const
    {
        return socket::apply_listen_options(fd_, options);
    }

    void Socket::set_keep_alive(const bool on) const
    {
        socket::set_keep_alive(fd_, on);
    }

    void Socket::set_none_block(const bool on) const
    {
        socket::set_none_block(fd_, on);
    }

    bool Socket::shutdown_write() const
    {
        return socket::shutdown_write(fd_);
    }

    void Socket::close()
    {
        if (fd_ >= 0) {
            socket::close_fd(fd_);
            fd_ = -1;
        }
    }
}
