#ifndef YUAN_RPC_PENDING_CALL_REGISTRY_H
#define YUAN_RPC_PENDING_CALL_REGISTRY_H

#include "types.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace yuan::rpc
{
    class PendingCallRegistry
    {
    public:
        using Clock = std::chrono::steady_clock;

        RequestId create(ResponseHandler handler,
                         std::chrono::milliseconds timeout,
                         Metadata metadata = {},
                         ContinuationId continuation_id = 0);

        bool complete(Response response);

        bool cancel(RequestId id, std::string error = "canceled");

        std::size_t expire(Clock::time_point now = Clock::now());

        [[nodiscard]] std::size_t size() const;

    private:
        struct PendingCall
        {
            ResponseHandler handler;
            Clock::time_point deadline{};
            Metadata metadata;
            ContinuationId continuation_id = 0;
        };

        std::optional<PendingCall> take(RequestId id);

        RequestId next_id_ = 1;
        mutable std::mutex mutex_;
        std::unordered_map<RequestId, PendingCall> calls_;
    };
}

#endif
