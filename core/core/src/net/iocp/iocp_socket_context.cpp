#include "net/iocp/iocp_socket_context.h"

#include "net/iocp/iocp_tcp_io.h"

#include <atomic>

namespace yuan::net
{
    namespace
    {
        uint64_t next_iocp_socket_generation() noexcept
        {
            static std::atomic_uint64_t next_generation{1};
            auto generation = next_generation.fetch_add(1, std::memory_order_relaxed);
            if (generation == 0) {
                generation = next_generation.fetch_add(1, std::memory_order_relaxed);
            }
            return generation == 0 ? 1 : generation;
        }
    }

    bool IocpSocketContext::attach(int fd, IocpCompletionPort &port, uintptr_t key) noexcept
    {
        reset();
        if (fd < 0 || !port.valid()) {
            return false;
        }

        if (!port.associate_socket(fd, key)) {
            return false;
        }

        fd_ = fd;
        key_ = key;
        port_ = &port;
        generation_ = next_iocp_socket_generation();
        closing_.store(false, std::memory_order_release);
        pending_operations_.store(0, std::memory_order_release);
        return true;
    }

    void IocpSocketContext::reset() noexcept
    {
        fd_ = -1;
        key_ = 0;
        port_ = nullptr;
        generation_ = 0;
        closing_.store(false, std::memory_order_release);
        pending_operations_.store(0, std::memory_order_release);
    }

    bool IocpSocketContext::valid() const noexcept
    {
        return fd_ >= 0 && port_ != nullptr && generation_ != 0;
    }

    int IocpSocketContext::fd() const noexcept
    {
        return fd_;
    }

    uintptr_t IocpSocketContext::key() const noexcept
    {
        return key_;
    }

    uint64_t IocpSocketContext::generation() const noexcept
    {
        return generation_;
    }

    uint32_t IocpSocketContext::pending_operations() const noexcept
    {
        return pending_operations_.load(std::memory_order_acquire);
    }

    bool IocpSocketContext::closing() const noexcept
    {
        return closing_.load(std::memory_order_acquire);
    }

    bool IocpSocketContext::begin_operation(IocpOperation &operation,
                                            IocpOperationKind kind,
                                            void *user_data) noexcept
    {
        if (!valid() || closing()) {
            return false;
        }

        pending_operations_.fetch_add(1, std::memory_order_acq_rel);
        if (closing()) {
            pending_operations_.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }

        operation.reset(kind, this, user_data, generation_);
        return true;
    }

    bool IocpSocketContext::complete_operation(const IocpOperation &operation) noexcept
    {
        uint32_t current = pending_operations_.load(std::memory_order_acquire);
        while (current != 0 &&
               !pending_operations_.compare_exchange_weak(current,
                                                          current - 1,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
        }
        return owns_completion(operation);
    }

    bool IocpSocketContext::owns_completion(const IocpOperation &operation) const noexcept
    {
        return operation.owner == this && operation.generation == generation_;
    }

    bool IocpSocketContext::request_close(bool cancel_io) noexcept
    {
        closing_.store(true, std::memory_order_release);
        if (!valid()) {
            return false;
        }
        if (!cancel_io || pending_operations() == 0) {
            return true;
        }
        return IocpTcpIo::cancel(fd_);
    }
}
