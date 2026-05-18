#ifndef __IOCP_ACCEPT_H__
#define __IOCP_ACCEPT_H__

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

namespace yuan::net
{
    inline constexpr std::size_t kIocpAcceptAddressBytes = sizeof(sockaddr_storage) + 16;
    inline constexpr std::size_t kIocpAcceptBufferBytes = kIocpAcceptAddressBytes * 2;

    struct IocpAcceptedAddresses
    {
        sockaddr_storage local{};
        sockaddr_storage remote{};
    };

    class IocpAcceptEx
    {
    public:
        bool load(int listener_fd) noexcept;
        bool loaded() const noexcept;

        bool post(int listener_fd,
                  int accepted_fd,
                  void *address_buffer,
                  std::size_t address_buffer_bytes,
                  void *operation,
                  uint32_t receive_data_bytes = 0) const noexcept;

        bool update_accept_context(int accepted_fd, int listener_fd) const noexcept;
        bool parse_addresses(void *address_buffer,
                             std::size_t address_buffer_bytes,
                             IocpAcceptedAddresses &addresses) const noexcept;

    private:
        void *accept_ex_ = nullptr;
        void *get_accept_ex_sockaddrs_ = nullptr;
    };
}

#endif
