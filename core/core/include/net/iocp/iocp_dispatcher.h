#ifndef __IOCP_DISPATCHER_H__
#define __IOCP_DISPATCHER_H__

#include "net/iocp/iocp_completion_port.h"
#include "net/iocp/iocp_operation.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

namespace yuan::net
{
    class IocpDispatcher
    {
    public:
        using CompletionCallback = std::function<void(const IocpCompletion &)>;
        using OperationCallback = std::function<void(IocpOperation &, const IocpCompletion &)>;

        IocpDispatcher() = default;
        ~IocpDispatcher();

        IocpDispatcher(const IocpDispatcher &) = delete;
        IocpDispatcher &operator=(const IocpDispatcher &) = delete;

        bool start(IocpCompletionPort &port, std::size_t worker_count, CompletionCallback callback);
        bool start_operations(IocpCompletionPort &port, std::size_t worker_count, OperationCallback callback);
        void stop();

        bool running() const noexcept;
        std::size_t worker_count() const noexcept;

    private:
        void worker_loop();

        IocpCompletionPort *port_ = nullptr;
        CompletionCallback callback_;
        OperationCallback operation_callback_;
        std::vector<std::thread> workers_;
        std::atomic_bool running_{false};
    };
}

#endif
