#include "net/iocp/iocp_connect.h"

#ifdef _WIN32
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
#endif

namespace yuan::net
{
    bool IocpConnectEx::load(int fd) noexcept
    {
#ifdef _WIN32
        if (fd < 0) {
            return false;
        }

        GUID connect_ex_guid = WSAID_CONNECTEX;
        DWORD bytes = 0;
        LPFN_CONNECTEX connect_ex = nullptr;
        if (::WSAIoctl(static_cast<SOCKET>(fd),
                       SIO_GET_EXTENSION_FUNCTION_POINTER,
                       &connect_ex_guid,
                       sizeof(connect_ex_guid),
                       &connect_ex,
                       sizeof(connect_ex),
                       &bytes,
                       nullptr,
                       nullptr) != 0) {
            return false;
        }

        connect_ex_ = reinterpret_cast<void *>(connect_ex);
        return true;
#else
        (void)fd;
        return false;
#endif
    }

    bool IocpConnectEx::loaded() const noexcept
    {
        return connect_ex_ != nullptr;
    }

    bool IocpConnectEx::post(int fd,
                             const sockaddr *remote_address,
                             int remote_address_bytes,
                             void *operation,
                             const void *send_buffer,
                             uint32_t send_bytes) const noexcept
    {
#ifdef _WIN32
        if (!loaded() || fd < 0 || !remote_address || remote_address_bytes <= 0 || !operation) {
            return false;
        }

        DWORD bytes = 0;
        auto *connect_ex = reinterpret_cast<LPFN_CONNECTEX>(connect_ex_);
        const BOOL ok = connect_ex(static_cast<SOCKET>(fd),
                                   remote_address,
                                   remote_address_bytes,
                                   const_cast<void *>(send_buffer),
                                   static_cast<DWORD>(send_bytes),
                                   &bytes,
                                   static_cast<LPOVERLAPPED>(operation));
        if (ok != FALSE) {
            return true;
        }

        return ::WSAGetLastError() == WSA_IO_PENDING;
#else
        (void)fd;
        (void)remote_address;
        (void)remote_address_bytes;
        (void)operation;
        (void)send_buffer;
        (void)send_bytes;
        return false;
#endif
    }

    bool IocpConnectEx::update_connect_context(int fd) const noexcept
    {
#ifdef _WIN32
        if (fd < 0) {
            return false;
        }

        return ::setsockopt(static_cast<SOCKET>(fd),
                            SOL_SOCKET,
                            SO_UPDATE_CONNECT_CONTEXT,
                            nullptr,
                            0) == 0;
#else
        (void)fd;
        return false;
#endif
    }
}
