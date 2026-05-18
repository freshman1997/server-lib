#ifndef __IOCP_TCP_IO_H__
#define __IOCP_TCP_IO_H__

#include <cstdint>

namespace yuan::net
{
    class IocpTcpIo
    {
    public:
        static bool post_recv(int fd,
                              void *buffer,
                              uint32_t buffer_bytes,
                              void *operation,
                              uint32_t *error = nullptr) noexcept;

        static bool post_send(int fd,
                              const void *buffer,
                              uint32_t buffer_bytes,
                              void *operation,
                              uint32_t *error = nullptr) noexcept;

        static bool cancel(int fd) noexcept;
    };
}

#endif
