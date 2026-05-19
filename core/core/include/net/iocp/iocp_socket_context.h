#ifndef __IOCP_SOCKET_CONTEXT_H__
#define __IOCP_SOCKET_CONTEXT_H__

#include "net/iocp/iocp_completion_port.h"
#include "net/iocp/iocp_operation.h"

#include <atomic>
#include <cstdint>

namespace yuan::net
{
    class IocpSocketContext
    {
    public:
        IocpSocketContext() = default;
        ~IocpSocketContext() = default;

        IocpSocketContext(const IocpSocketContext &) = delete;
        IocpSocketContext &operator=(const IocpSocketContext &) = delete;

        bool attach(int fd, IocpCompletionPort &port, uintptr_t key = 0) noexcept;
        void reset() noexcept;

        bool valid() const noexcept;
        int fd() const noexcept;
        uintptr_t key() const noexcept;
        uint64_t generation() const noexcept;
        uint32_t pending_operations() const noexcept;
        bool closing() const noexcept;

        bool begin_operation(IocpOperation &operation,
                             IocpOperationKind kind,
                             void *user_data = nullptr) noexcept;
        bool complete_operation(const IocpOperation &operation) noexcept;
        bool owns_completion(const IocpOperation &operation) const noexcept;

        bool request_close(bool cancel_io = true) noexcept;

    private:
        int fd_ = -1;
        uintptr_t key_ = 0;
        IocpCompletionPort *port_ = nullptr;
        std::atomic_uint32_t pending_operations_{0};
        std::atomic_bool closing_{false};
        uint64_t generation_ = 0;
    };
}

#endif
