#include "net/iocp/iocp_dispatcher.h"

#include <algorithm>
#include <cstdint>

namespace yuan::net
{
    namespace
    {
        constexpr uint32_t kInfiniteTimeoutMs = 0xFFFFFFFFU;
    }

    IocpDispatcher::~IocpDispatcher()
    {
        stop();
    }

    bool IocpDispatcher::start(IocpCompletionPort &port,
                               std::size_t worker_count,
                               CompletionCallback callback)
    {
        stop();
        if (!port.valid() || !callback) {
            return false;
        }

        port_ = &port;
        callback_ = std::move(callback);
        operation_callback_ = nullptr;
        const std::size_t actual_workers = (std::max<std::size_t>)(1, worker_count);
        workers_.reserve(actual_workers);
        running_.store(true, std::memory_order_release);
        try {
            for (std::size_t i = 0; i < actual_workers; ++i) {
                workers_.emplace_back([this]() { worker_loop(); });
            }
        } catch (...) {
            stop();
            return false;
        }
        return true;
    }

    bool IocpDispatcher::start_operations(IocpCompletionPort &port,
                                          std::size_t worker_count,
                                          OperationCallback callback)
    {
        stop();
        if (!port.valid() || !callback) {
            return false;
        }

        port_ = &port;
        callback_ = nullptr;
        operation_callback_ = std::move(callback);
        const std::size_t actual_workers = (std::max<std::size_t>)(1, worker_count);
        workers_.reserve(actual_workers);
        running_.store(true, std::memory_order_release);
        try {
            for (std::size_t i = 0; i < actual_workers; ++i) {
                workers_.emplace_back([this]() { worker_loop(); });
            }
        } catch (...) {
            stop();
            return false;
        }
        return true;
    }

    void IocpDispatcher::stop()
    {
        if (!running_.exchange(false, std::memory_order_acq_rel) && workers_.empty()) {
            callback_ = nullptr;
            port_ = nullptr;
            return;
        }

        if (port_) {
            for (std::size_t i = 0; i < workers_.size(); ++i) {
                port_->post();
            }
        }

        for (auto &worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
        callback_ = nullptr;
        operation_callback_ = nullptr;
        port_ = nullptr;
    }

    bool IocpDispatcher::running() const noexcept
    {
        return running_.load(std::memory_order_acquire);
    }

    std::size_t IocpDispatcher::worker_count() const noexcept
    {
        return workers_.size();
    }

    void IocpDispatcher::worker_loop()
    {
        for (;;) {
            IocpCompletion completion;
            auto *port = port_;
            if (!port || !port->wait(kInfiniteTimeoutMs, completion)) {
                if (!running_.load(std::memory_order_acquire)) {
                    break;
                }
                continue;
            }

            if (!completion.operation) {
                break;
            }

            auto operation_callback = operation_callback_;
            if (operation_callback) {
                if (auto *operation = IocpOperation::from_completion(completion)) {
                    operation_callback(*operation, completion);
                }
                continue;
            }

            auto completion_callback = callback_;
            if (completion_callback) {
                completion_callback(completion);
            }
        }
    }
}
