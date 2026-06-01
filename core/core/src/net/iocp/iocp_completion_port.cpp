#include "net/iocp/iocp_completion_port.h"

#include <algorithm>

#include "native_platform.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

namespace yuan::net
{
    IocpCompletionPort::~IocpCompletionPort()
    {
        close();
    }

    IocpCompletionPort::IocpCompletionPort(IocpCompletionPort &&other) noexcept
        : handle_(other.handle_)
    {
        other.handle_ = nullptr;
    }

    IocpCompletionPort &IocpCompletionPort::operator=(IocpCompletionPort &&other) noexcept
    {
        if (this != &other) {
            close();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    bool IocpCompletionPort::init(uint32_t concurrency)
    {
        close();
#ifdef _WIN32
        handle_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE,
                                           nullptr,
                                           0,
                                           static_cast<DWORD>(concurrency));
        return handle_ != nullptr;
#else
        (void)concurrency;
        return false;
#endif
    }

    bool IocpCompletionPort::valid() const noexcept
    {
        return handle_ != nullptr;
    }

    void IocpCompletionPort::close() noexcept
    {
#ifdef _WIN32
        if (handle_) {
            ::CloseHandle(static_cast<HANDLE>(handle_));
            handle_ = nullptr;
        }
#else
        handle_ = nullptr;
#endif
    }

    bool IocpCompletionPort::associate_socket(int fd, uintptr_t key) noexcept
    {
#ifdef _WIN32
        if (!handle_ || fd < 0) {
            return false;
        }

        const auto socket_handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(fd));
        return ::CreateIoCompletionPort(socket_handle,
                                        static_cast<HANDLE>(handle_),
                                        static_cast<ULONG_PTR>(key),
                                        0) != nullptr;
#else
        (void)fd;
        (void)key;
        return false;
#endif
    }

    bool IocpCompletionPort::post(uintptr_t key, void *operation, uint32_t bytes) noexcept
    {
#ifdef _WIN32
        if (!handle_) {
            return false;
        }

        return ::PostQueuedCompletionStatus(static_cast<HANDLE>(handle_),
                                            static_cast<DWORD>(bytes),
                                            static_cast<ULONG_PTR>(key),
                                            static_cast<LPOVERLAPPED>(operation)) != FALSE;
#else
        (void)key;
        (void)operation;
        (void)bytes;
        return false;
#endif
    }

    bool IocpCompletionPort::wait(uint32_t timeout_ms, IocpCompletion &completion) noexcept
    {
        completion = {};
#ifdef _WIN32
        if (!handle_) {
            completion.error = ERROR_INVALID_HANDLE;
            return false;
        }

        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED operation = nullptr;
        const BOOL ok = ::GetQueuedCompletionStatus(static_cast<HANDLE>(handle_),
                                                    &bytes,
                                                    &key,
                                                    &operation,
                                                    static_cast<DWORD>(timeout_ms));
        completion.bytes = static_cast<uint32_t>(bytes);
        completion.key = static_cast<uintptr_t>(key);
        completion.operation = operation;
        completion.ok = ok != FALSE;
        completion.error = completion.ok ? 0U : static_cast<uint32_t>(app::GetLastSystemError());
        return completion.ok || completion.operation != nullptr;
#else
        (void)timeout_ms;
        return false;
#endif
    }

    bool IocpCompletionPort::wait_many(uint32_t timeout_ms,
                                       IocpCompletion *completions,
                                       std::size_t max_completions,
                                       std::size_t &completion_count) noexcept
    {
        completion_count = 0;
#ifdef _WIN32
        if (!handle_ || !completions || max_completions == 0) {
            return false;
        }

        constexpr std::size_t kMaxBatch = 32;
        OVERLAPPED_ENTRY entries[kMaxBatch]{};
        const ULONG capacity = static_cast<ULONG>((std::min)(max_completions, kMaxBatch));
        ULONG removed = 0;
        const BOOL ok = ::GetQueuedCompletionStatusEx(static_cast<HANDLE>(handle_),
                                                      entries,
                                                      capacity,
                                                      &removed,
                                                      static_cast<DWORD>(timeout_ms),
                                                      FALSE);
        if (!ok || removed == 0) {
            return false;
        }

        for (ULONG i = 0; i < removed; ++i) {
            auto &out = completions[i];
            out.bytes = static_cast<uint32_t>(entries[i].dwNumberOfBytesTransferred);
            out.key = static_cast<uintptr_t>(entries[i].lpCompletionKey);
            out.operation = entries[i].lpOverlapped;
            out.ok = true;
            out.error = 0;
        }
        completion_count = removed;
        return true;
#else
        (void)timeout_ms;
        (void)completions;
        (void)max_completions;
        return false;
#endif
    }

    void *IocpCompletionPort::native_handle() const noexcept
    {
        return handle_;
    }
}
