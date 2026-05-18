#include "net/iocp/iocp_completion_port.h"

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
        completion.error = completion.ok ? 0U : static_cast<uint32_t>(::GetLastError());
        return completion.ok || completion.operation != nullptr;
#else
        (void)timeout_ms;
        return false;
#endif
    }

    void *IocpCompletionPort::native_handle() const noexcept
    {
        return handle_;
    }
}
