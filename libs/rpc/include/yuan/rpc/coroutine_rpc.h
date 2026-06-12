#ifndef YUAN_RPC_COROUTINE_RPC_H
#define YUAN_RPC_COROUTINE_RPC_H

#include "client.h"
#include "codec.h"

#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace yuan::rpc
{
    template<typename T>
    class RpcTask
    {
    public:
        struct promise_type
        {
            std::optional<T> value;
            std::exception_ptr exception;

            RpcTask get_return_object()
            {
                return RpcTask(std::coroutine_handle<promise_type>::from_promise(*this));
            }

            std::suspend_always initial_suspend() noexcept
            {
                return {};
            }

            std::suspend_always final_suspend() noexcept
            {
                return {};
            }

            void unhandled_exception()
            {
                exception = std::current_exception();
            }

            void return_value(T result)
            {
                value = std::move(result);
            }
        };

        explicit RpcTask(std::coroutine_handle<promise_type> handle)
            : handle_(handle)
        {
        }

        RpcTask(RpcTask &&other) noexcept
            : handle_(other.handle_)
        {
            other.handle_ = {};
        }

        RpcTask(const RpcTask &) = delete;
        RpcTask &operator=(const RpcTask &) = delete;

        ~RpcTask()
        {
            if (handle_) {
                handle_.destroy();
            }
        }

        [[nodiscard]] bool done() const
        {
            return !handle_ || handle_.done();
        }

        void resume()
        {
            if (handle_ && !handle_.done()) {
                handle_.resume();
            }
        }

        T result()
        {
            while (handle_ && !handle_.done()) {
                handle_.resume();
            }
            if (handle_.promise().exception) {
                std::rethrow_exception(handle_.promise().exception);
            }
            return std::move(*handle_.promise().value);
        }

    private:
        std::coroutine_handle<promise_type> handle_;
    };

    struct RpcAwaitResult
    {
        RpcStatus status = RpcStatus::internal_error;
        std::string error;
        Bytes payload;
        Metadata metadata;
    };

    class RpcCoroutineRegistry
    {
    public:
        ContinuationId next_id()
        {
            ContinuationId id = next_id_.fetch_add(1, std::memory_order_relaxed);
            while (id == 0) {
                id = next_id_.fetch_add(1, std::memory_order_relaxed);
            }
            return id;
        }

        bool suspend(ContinuationId id, std::coroutine_handle<> handle)
        {
            if (id == 0 || !handle) {
                return false;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            return entries_.emplace(id, Entry{handle, std::nullopt}).second;
        }

        bool complete(Response response)
        {
            std::coroutine_handle<> handle;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = entries_.find(response.coroutine_id);
                if (it == entries_.end()) {
                    return false;
                }
                it->second.result = RpcAwaitResult{response.status, std::move(response.error), std::move(response.payload), std::move(response.metadata)};
                handle = it->second.handle;
            }
            handle.resume();
            return true;
        }

        bool cancel(ContinuationId id, std::string error)
        {
            Response response;
            response.coroutine_id = id;
            response.status = RpcStatus::canceled;
            response.error = std::move(error);
            return complete(std::move(response));
        }

        std::optional<RpcAwaitResult> take(ContinuationId id)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = entries_.find(id);
            if (it == entries_.end() || !it->second.result.has_value()) {
                return std::nullopt;
            }
            auto result = std::move(it->second.result);
            entries_.erase(it);
            return result;
        }

        [[nodiscard]] std::size_t size() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return entries_.size();
        }

    private:
        struct Entry
        {
            std::coroutine_handle<> handle;
            std::optional<RpcAwaitResult> result;
        };

        std::atomic<ContinuationId> next_id_{1};
        mutable std::mutex mutex_;
        std::unordered_map<ContinuationId, Entry> entries_;
    };

    template<typename ResponseT>
    class RpcCallAwaiter
    {
    public:
        RpcCallAwaiter(Client &client, RpcCoroutineRegistry &registry, Route route, Bytes payload, CallOptions options)
            : client_(client), registry_(registry), route_(std::move(route)), payload_(std::move(payload)), options_(std::move(options))
        {
            if (options_.coroutine_id == 0) {
                options_.coroutine_id = registry_.next_id();
            }
        }

        bool await_ready() const noexcept
        {
            return false;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            if (!registry_.suspend(options_.coroutine_id, handle)) {
                result_ = RpcAwaitResult{RpcStatus::internal_error, "duplicate rpc coroutine id", {}, {}};
                return false;
            }
            const ContinuationId id = options_.continuation_id();
            const bool sent = client_.call(
                std::move(route_),
                std::move(payload_),
                [this](Response response) {
                    (void)registry_.complete(std::move(response));
                },
                options_);
            if (!sent) {
                (void)registry_.cancel(id, "rpc send failed");
            }
            return true;
        }

        ResponseT await_resume()
        {
            auto result = registry_.take(options_.coroutine_id);
            if (!result.has_value()) {
                result = std::move(result_);
            }
            if (!result.has_value()) {
                throw RpcError(RpcStatus::internal_error, "rpc coroutine resumed without result");
            }
            if (result->status != RpcStatus::ok) {
                throw RpcError(result->status, result->error);
            }
            return Codec<ResponseT>::decode(result->payload);
        }

    private:
        Client &client_;
        RpcCoroutineRegistry &registry_;
        Route route_;
        Bytes payload_;
        CallOptions options_;
        std::optional<RpcAwaitResult> result_;
    };

    class CoroutineRpcClient
    {
    public:
        explicit CoroutineRpcClient(Client &client, RpcCoroutineRegistry &registry)
            : client_(client), registry_(registry)
        {
        }

        template<typename Request, typename ResponseT>
        RpcCallAwaiter<ResponseT> call_async(Route route, const Request &request, CallOptions options = {})
        {
            options.serialization = CodecTraits<Request>::serialization;
            return RpcCallAwaiter<ResponseT>(client_, registry_, std::move(route), Codec<Request>::encode(request), std::move(options));
        }

    private:
        Client &client_;
        RpcCoroutineRegistry &registry_;
    };
}

#endif
