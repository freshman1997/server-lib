#ifndef __IOCP_COMPLETION_PORT_H__
#define __IOCP_COMPLETION_PORT_H__

#include <cstddef>
#include <cstdint>

namespace yuan::net
{
    struct IocpCompletion
    {
        uint32_t bytes = 0;
        uintptr_t key = 0;
        void *operation = nullptr;
        uint32_t error = 0;
        bool ok = false;
    };

    class IocpCompletionPort
    {
    public:
        IocpCompletionPort() = default;
        ~IocpCompletionPort();

        IocpCompletionPort(const IocpCompletionPort &) = delete;
        IocpCompletionPort &operator=(const IocpCompletionPort &) = delete;

        IocpCompletionPort(IocpCompletionPort &&other) noexcept;
        IocpCompletionPort &operator=(IocpCompletionPort &&other) noexcept;

        bool init(uint32_t concurrency = 0);
        bool valid() const noexcept;
        void close() noexcept;

        bool associate_socket(int fd, uintptr_t key) noexcept;
        bool post(uintptr_t key = 0, void *operation = nullptr, uint32_t bytes = 0) noexcept;
        bool wait(uint32_t timeout_ms, IocpCompletion &completion) noexcept;
        bool wait_many(uint32_t timeout_ms,
                       IocpCompletion *completions,
                       std::size_t max_completions,
                       std::size_t &completion_count) noexcept;

        void *native_handle() const noexcept;

    private:
        void *handle_ = nullptr;
    };
}

#endif
