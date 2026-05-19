#ifndef __IOCP_CONNECT_H__
#define __IOCP_CONNECT_H__

#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

namespace yuan::net
{
    class IocpConnectEx
    {
    public:
        bool load(int fd) noexcept;
        bool loaded() const noexcept;

        bool post(int fd,
                  const sockaddr *remote_address,
                  int remote_address_bytes,
                  void *operation,
                  const void *send_buffer = nullptr,
                  uint32_t send_bytes = 0) const noexcept;

        bool update_connect_context(int fd) const noexcept;

    private:
        void *connect_ex_ = nullptr;
    };
}

#endif
