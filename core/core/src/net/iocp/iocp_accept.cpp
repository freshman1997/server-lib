#include "net/iocp/iocp_accept.h"

#ifdef _WIN32
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
#endif

#include <cstring>

#include "native_platform.h"

namespace yuan::net
{
    bool IocpAcceptEx::load(int listener_fd) noexcept
    {
#ifdef _WIN32
        if (listener_fd < 0) {
            return false;
        }

        GUID accept_ex_guid = WSAID_ACCEPTEX;
        GUID get_addrs_guid = WSAID_GETACCEPTEXSOCKADDRS;
        DWORD bytes = 0;
        LPFN_ACCEPTEX accept_ex = nullptr;
        LPFN_GETACCEPTEXSOCKADDRS get_addrs = nullptr;

        if (::WSAIoctl(static_cast<SOCKET>(listener_fd),
                       SIO_GET_EXTENSION_FUNCTION_POINTER,
                       &accept_ex_guid,
                       sizeof(accept_ex_guid),
                       &accept_ex,
                       sizeof(accept_ex),
                       &bytes,
                       nullptr,
                       nullptr) != 0) {
            return false;
        }

        if (::WSAIoctl(static_cast<SOCKET>(listener_fd),
                       SIO_GET_EXTENSION_FUNCTION_POINTER,
                       &get_addrs_guid,
                       sizeof(get_addrs_guid),
                       &get_addrs,
                       sizeof(get_addrs),
                       &bytes,
                       nullptr,
                       nullptr) != 0) {
            return false;
        }

        accept_ex_ = reinterpret_cast<void *>(accept_ex);
        get_accept_ex_sockaddrs_ = reinterpret_cast<void *>(get_addrs);
        return true;
#else
        (void)listener_fd;
        return false;
#endif
    }

    bool IocpAcceptEx::loaded() const noexcept
    {
        return accept_ex_ != nullptr && get_accept_ex_sockaddrs_ != nullptr;
    }

    bool IocpAcceptEx::post(int listener_fd,
                            int accepted_fd,
                            void *address_buffer,
                            std::size_t address_buffer_bytes,
                            void *operation,
                            uint32_t receive_data_bytes) const noexcept
    {
#ifdef _WIN32
        if (!loaded() || listener_fd < 0 || accepted_fd < 0 || !address_buffer ||
            address_buffer_bytes < kIocpAcceptBufferBytes || !operation) {
            return false;
        }

        DWORD bytes = 0;
        auto *accept_ex = reinterpret_cast<LPFN_ACCEPTEX>(accept_ex_);
        const BOOL ok = accept_ex(static_cast<SOCKET>(listener_fd),
                                  static_cast<SOCKET>(accepted_fd),
                                  address_buffer,
                                  static_cast<DWORD>(receive_data_bytes),
                                  static_cast<DWORD>(kIocpAcceptAddressBytes),
                                  static_cast<DWORD>(kIocpAcceptAddressBytes),
                                  &bytes,
                                  static_cast<LPOVERLAPPED>(operation));
        if (ok != FALSE) {
            return true;
        }

        return app::GetLastNativeError() == WSA_IO_PENDING;
#else
        (void)listener_fd;
        (void)accepted_fd;
        (void)address_buffer;
        (void)address_buffer_bytes;
        (void)operation;
        (void)receive_data_bytes;
        return false;
#endif
    }

    bool IocpAcceptEx::update_accept_context(int accepted_fd, int listener_fd) const noexcept
    {
#ifdef _WIN32
        if (accepted_fd < 0 || listener_fd < 0) {
            return false;
        }

        const SOCKET listener = static_cast<SOCKET>(listener_fd);
        return ::setsockopt(static_cast<SOCKET>(accepted_fd),
                            SOL_SOCKET,
                            SO_UPDATE_ACCEPT_CONTEXT,
                            reinterpret_cast<const char *>(&listener),
                            sizeof(listener)) == 0;
#else
        (void)accepted_fd;
        (void)listener_fd;
        return false;
#endif
    }

    bool IocpAcceptEx::parse_addresses(void *address_buffer,
                                       std::size_t address_buffer_bytes,
                                       IocpAcceptedAddresses &addresses) const noexcept
    {
#ifdef _WIN32
        if (!loaded() || !address_buffer || address_buffer_bytes < kIocpAcceptBufferBytes) {
            return false;
        }

        sockaddr *local = nullptr;
        sockaddr *remote = nullptr;
        int local_len = 0;
        int remote_len = 0;
        auto *get_addrs = reinterpret_cast<LPFN_GETACCEPTEXSOCKADDRS>(get_accept_ex_sockaddrs_);
        get_addrs(address_buffer,
                  0,
                  static_cast<DWORD>(kIocpAcceptAddressBytes),
                  static_cast<DWORD>(kIocpAcceptAddressBytes),
                  &local,
                  &local_len,
                  &remote,
                  &remote_len);

        if (!local || !remote || local_len <= 0 || remote_len <= 0 ||
            static_cast<std::size_t>(local_len) > sizeof(sockaddr_storage) ||
            static_cast<std::size_t>(remote_len) > sizeof(sockaddr_storage)) {
            return false;
        }

        std::memset(&addresses, 0, sizeof(addresses));
        std::memcpy(&addresses.local, local, static_cast<std::size_t>(local_len));
        std::memcpy(&addresses.remote, remote, static_cast<std::size_t>(remote_len));
        return true;
#else
        (void)address_buffer;
        (void)address_buffer_bytes;
        (void)addresses;
        return false;
#endif
    }
}
