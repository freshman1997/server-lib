#include "net/iocp/iocp_tcp_io.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

namespace yuan::net
{
    bool IocpTcpIo::post_recv(int fd,
                              void *buffer,
                              uint32_t buffer_bytes,
                              void *operation,
                              uint32_t *error) noexcept
    {
        if (error) {
            *error = 0;
        }
#ifdef _WIN32
        if (fd < 0 || !buffer || buffer_bytes == 0 || !operation) {
            if (error) {
                *error = WSAEINVAL;
            }
            return false;
        }

        WSABUF wsa_buffer{};
        wsa_buffer.buf = static_cast<char *>(buffer);
        wsa_buffer.len = static_cast<ULONG>(buffer_bytes);
        DWORD flags = 0;
        DWORD bytes = 0;
        const int rc = ::WSARecv(static_cast<SOCKET>(fd),
                                 &wsa_buffer,
                                 1,
                                 &bytes,
                                 &flags,
                                 static_cast<LPWSAOVERLAPPED>(operation),
                                 nullptr);
        if (rc == 0) {
            return true;
        }

        const int last_error = ::WSAGetLastError();
        if (error) {
            *error = static_cast<uint32_t>(last_error);
        }
        return last_error == WSA_IO_PENDING;
#else
        (void)fd;
        (void)buffer;
        (void)buffer_bytes;
        (void)operation;
        return false;
#endif
    }

    bool IocpTcpIo::post_send(int fd,
                              const void *buffer,
                              uint32_t buffer_bytes,
                              void *operation,
                              uint32_t *error) noexcept
    {
        if (error) {
            *error = 0;
        }
#ifdef _WIN32
        if (fd < 0 || !buffer || buffer_bytes == 0 || !operation) {
            if (error) {
                *error = WSAEINVAL;
            }
            return false;
        }

        WSABUF wsa_buffer{};
        wsa_buffer.buf = const_cast<char *>(static_cast<const char *>(buffer));
        wsa_buffer.len = static_cast<ULONG>(buffer_bytes);
        DWORD bytes = 0;
        const int rc = ::WSASend(static_cast<SOCKET>(fd),
                                 &wsa_buffer,
                                 1,
                                 &bytes,
                                 0,
                                 static_cast<LPWSAOVERLAPPED>(operation),
                                 nullptr);
        if (rc == 0) {
            return true;
        }

        const int last_error = ::WSAGetLastError();
        if (error) {
            *error = static_cast<uint32_t>(last_error);
        }
        return last_error == WSA_IO_PENDING;
#else
        (void)fd;
        (void)buffer;
        (void)buffer_bytes;
        (void)operation;
        return false;
#endif
    }

    bool IocpTcpIo::cancel(int fd) noexcept
    {
#ifdef _WIN32
        if (fd < 0) {
            return false;
        }

        const BOOL ok = ::CancelIoEx(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(fd)), nullptr);
        if (ok != FALSE) {
            return true;
        }
        return ::GetLastError() == ERROR_NOT_FOUND;
#else
        (void)fd;
        return false;
#endif
    }
}
