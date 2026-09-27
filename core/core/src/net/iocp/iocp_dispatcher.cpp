#include "net/iocp/iocp_dispatcher.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace yuan::net
{
    namespace
    {
        constexpr uint32_t kInfiniteTimeoutMs = 0xFFFFFFFFU;
        constexpr uintptr_t kStopCompletionKey = static_cast<uintptr_t>(-1);
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
                port_->post(kStopCompletionKey);
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

    void IocpDispatcher::set_completion_batch_size(std::size_t batch_size) noexcept
    {
        completion_batch_size_ = (std::max<std::size_t>)(1, batch_size);
    }

    std::size_t IocpDispatcher::worker_count() const noexcept
    {
        return workers_.size();
    }

    void IocpDispatcher::worker_loop()
    {
        if (completion_batch_size_ <= 1) {
            worker_loop_single();
        } else {
            worker_loop_batched();
        }
    }

    void IocpDispatcher::worker_loop_single()
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

            if (!completion.operation && completion.key == kStopCompletionKey) {
                break;
            }
            if (!completion.operation) {
                continue;
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

    void IocpDispatcher::worker_loop_batched()
    {
        constexpr std::size_t kMaxBatchSize = 32;

        for (;;) {
            auto *port = port_;
            if (!port) {
                return;
            }

            IocpCompletion first;
            if (!port->wait(kInfiniteTimeoutMs, first)) {
                if (!running_.load(std::memory_order_acquire)) {
                    return;
                }
                continue;
            }

            std::array<IocpCompletion, kMaxBatchSize> completions{};
            completions[0] = first;
            std::size_t count = 1;
            const auto capacity = (std::min)(completion_batch_size_ - 1, kMaxBatchSize - 1);
            if (capacity != 0) {
                std::size_t drained = 0;
                if (port->wait_many(0, completions.data() + 1, capacity, drained)) {
                    count += drained;
                }
            }

            bool stop = false;
            std::size_t extra_stop_tokens = 0;
            for (std::size_t i = 0; i < count; ++i) {
                auto &completion = completions[i];
                if (!completion.operation) {
                    if (completion.key == kStopCompletionKey) {
                        if (stop) {
                            ++extra_stop_tokens;
                        } else {
                            stop = true;
                        }
                    }
                    continue;
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

            for (std::size_t i = 0; i < extra_stop_tokens; ++i) {
                port->post(kStopCompletionKey);
            }
            if (stop) {
                return;
            }
        }
    }

}
