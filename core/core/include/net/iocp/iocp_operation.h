#ifndef __IOCP_OPERATION_H__
#define __IOCP_OPERATION_H__

#include "net/iocp/iocp_completion_port.h"

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

namespace yuan::net
{
    enum class IocpOperationKind
    {
        user,
        accept,
        recv,
        send,
        connect,
        close
    };

#ifdef _WIN32
    using IocpNativeOverlapped = OVERLAPPED;
#else
    struct IocpNativeOverlapped
    {
        uintptr_t opaque[4]{};
    };
#endif

    struct IocpOperation
    {
        IocpNativeOverlapped overlapped{};
        IocpOperationKind kind = IocpOperationKind::user;
        void *owner = nullptr;
        void *user_data = nullptr;
        uint64_t generation = 0;

        void reset(IocpOperationKind next_kind,
                   void *next_owner = nullptr,
                   void *next_user_data = nullptr,
                   uint64_t next_generation = 0) noexcept
        {
            overlapped = {};
            kind = next_kind;
            owner = next_owner;
            user_data = next_user_data;
            generation = next_generation;
        }

        void *native_overlapped() noexcept
        {
            return &overlapped;
        }

        const void *native_overlapped() const noexcept
        {
            return &overlapped;
        }

        static IocpOperation *from_native(void *native) noexcept
        {
            return static_cast<IocpOperation *>(native);
        }

        static const IocpOperation *from_native(const void *native) noexcept
        {
            return static_cast<const IocpOperation *>(native);
        }

        static IocpOperation *from_completion(const IocpCompletion &completion) noexcept
        {
            return from_native(completion.operation);
        }
    };

    static_assert(offsetof(IocpOperation, overlapped) == 0,
                  "IocpOperation must begin with native OVERLAPPED storage");
}

#endif
