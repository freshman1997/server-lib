#include "yuan/rpc/types.h"

#include <utility>

namespace yuan::rpc
{
    std::optional<std::string> RpcContext::metadata_value(std::string_view key) const
    {
        const auto it = metadata.find(std::string(key));
        if (it == metadata.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    RpcContext context_from(const Message &message)
    {
        return RpcContext{message.request_id, message.continuation_id(), message.source, message.target, message.metadata};
    }

    RpcContext context_from(const Response &response)
    {
        RpcContext context;
        context.request_id = response.request_id;
        context.continuation_id = response.continuation_id();
        context.metadata = response.metadata;
        return context;
    }

    RpcError::RpcError(RpcStatus status, std::string message)
        : std::runtime_error(message.empty() ? default_message(status) : std::move(message)),
          status_(status)
    {
    }

    RpcStatus RpcError::status() const
    {
        return status_;
    }

    std::string RpcError::default_message(RpcStatus status)
    {
        return std::string(to_string(status));
    }

    std::string_view to_string(RpcStatus status)
    {
        switch (status) {
            case RpcStatus::ok:
                return "ok";
            case RpcStatus::not_found:
                return "not_found";
            case RpcStatus::timeout:
                return "timeout";
            case RpcStatus::canceled:
                return "canceled";
            case RpcStatus::bad_request:
                return "bad_request";
            case RpcStatus::unavailable:
                return "unavailable";
            case RpcStatus::internal_error:
                return "internal_error";
        }
        return "unknown";
    }

    std::string route_key(const Route &route)
    {
        if (route.service != 0 || route.method != 0) {
            return std::to_string(route.service) + ":" + std::to_string(route.method);
        }
        return route.name;
    }
}
