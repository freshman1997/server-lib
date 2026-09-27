#ifndef YUAN_RPC_LOCAL_BUS_H
#define YUAN_RPC_LOCAL_BUS_H

#include "types.h"

#include <mutex>
#include <memory>
#include <unordered_map>

namespace yuan::rpc
{
    class LocalBus
    {
    public:
        bool bind(Route route, RequestHandler handler);

        bool unbind(const Route &route);

        Response dispatch(const Message &message) const;

        [[nodiscard]] std::size_t size() const;

    private:
        using NumericRouteKey = std::uint64_t;

        static NumericRouteKey numeric_route_key(const Route &route) noexcept;
        static bool uses_numeric_route(const Route &route) noexcept;

        using HandlerPtr = std::shared_ptr<const RequestHandler>;

        mutable std::mutex mutex_;
        std::unordered_map<NumericRouteKey, HandlerPtr> numeric_handlers_;
        std::unordered_map<std::string, HandlerPtr> named_handlers_;
    };
}

#endif
