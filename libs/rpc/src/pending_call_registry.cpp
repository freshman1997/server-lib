#include "yuan/rpc/pending_call_registry.h"

#include <utility>

namespace yuan::rpc
{
    RequestId PendingCallRegistry::create(ResponseHandler handler,
                                          std::chrono::milliseconds timeout,
                                          Metadata metadata,
                                          ContinuationId continuation_id)
    {
        if (!handler) {
            return 0;
        }

        PendingCall call;
        call.handler = std::move(handler);
        call.metadata = std::move(metadata);
        call.continuation_id = continuation_id;
        if (timeout.count() > 0) {
            call.deadline = Clock::now() + timeout;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        RequestId id = next_id_++;
        while (id == 0) {
            id = next_id_++;
        }
        calls_.emplace(id, std::move(call));
        return id;
    }

    bool PendingCallRegistry::complete(Response response)
    {
        auto call = take(response.request_id);
        if (!call.has_value()) {
            return false;
        }
        if (response.continuation_id() == 0) {
            response.set_continuation_id(call->continuation_id);
        }
        call->handler(std::move(response));
        return true;
    }

    bool PendingCallRegistry::cancel(RequestId id, std::string error)
    {
        auto call = take(id);
        if (!call.has_value()) {
            return false;
        }

        Response response;
        response.request_id = id;
        response.set_continuation_id(call->continuation_id);
        response.status = RpcStatus::canceled;
        response.error = std::move(error);
        call->handler(std::move(response));
        return true;
    }

    std::size_t PendingCallRegistry::expire(Clock::time_point now)
    {
        std::vector<std::pair<RequestId, PendingCall>> expired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = calls_.begin(); it != calls_.end();) {
                if (it->second.deadline != Clock::time_point{} && it->second.deadline <= now) {
                    expired.emplace_back(it->first, std::move(it->second));
                    it = calls_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (auto &[id, call] : expired) {
            Response response;
            response.request_id = id;
            response.set_continuation_id(call.continuation_id);
            response.status = RpcStatus::timeout;
            response.error = "timeout";
            call.handler(std::move(response));
        }
        return expired.size();
    }

    std::size_t PendingCallRegistry::size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_.size();
    }

    std::optional<PendingCallRegistry::PendingCall> PendingCallRegistry::take(RequestId id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = calls_.find(id);
        if (it == calls_.end()) {
            return std::nullopt;
        }
        auto call = std::move(it->second);
        calls_.erase(it);
        return call;
    }
}
