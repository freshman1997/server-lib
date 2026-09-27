#ifndef __IOCP_TCP_IO_H__
#define __IOCP_TCP_IO_H__

#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#endif


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

#ifdef _WIN32
        static bool post_send_many(int fd,
                                   WSABUF *buffers,
                                   DWORD buffer_count,
                                   void *operation,
                                   uint32_t *error = nullptr) noexcept;
#endif


        static bool cancel(int fd) noexcept;
    };
}

#endif
