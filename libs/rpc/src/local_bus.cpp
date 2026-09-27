#include "yuan/rpc/local_bus.h"

#include "yuan/rpc/profiling.h"

#include <exception>
#include <utility>

namespace yuan::rpc
{
    LocalBus::NumericRouteKey LocalBus::numeric_route_key(const Route &route) noexcept
    {
        return (static_cast<NumericRouteKey>(route.service) << 32U) | static_cast<NumericRouteKey>(route.method);
    }

    bool LocalBus::uses_numeric_route(const Route &route) noexcept
    {
        return route.service != 0 || route.method != 0;
    }

    bool LocalBus::bind(Route route, RequestHandler handler)
    {
        YUAN_RPC_PROFILE_ZONE("yuan.rpc.local_bus.bind");
        if (!route.valid() || !handler) {
            return false;
        }

        auto handler_ptr = std::make_shared<const RequestHandler>(std::move(handler));
        std::lock_guard<std::mutex> lock(mutex_);
        if (uses_numeric_route(route)) {
            return numeric_handlers_.emplace(numeric_route_key(route), std::move(handler_ptr)).second;
        }
        return named_handlers_.emplace(std::move(route.name), std::move(handler_ptr)).second;
    }

    bool LocalBus::unbind(const Route &route)
    {
        YUAN_RPC_PROFILE_ZONE("yuan.rpc.local_bus.unbind");
        std::lock_guard<std::mutex> lock(mutex_);
        if (uses_numeric_route(route)) {
            return numeric_handlers_.erase(numeric_route_key(route)) != 0;
        }
        return named_handlers_.erase(route.name) != 0;
    }

    Response LocalBus::dispatch(const Message &message) const
    {
        YUAN_RPC_PROFILE_ZONE("yuan.rpc.local_bus.dispatch");
        HandlerPtr handler;
        {
            YUAN_RPC_PROFILE_ZONE("yuan.rpc.local_bus.lookup");
            std::lock_guard<std::mutex> lock(mutex_);
            if (uses_numeric_route(message.route)) {
                const auto it = numeric_handlers_.find(numeric_route_key(message.route));
                if (it != numeric_handlers_.end()) {
                    handler = it->second;
                }
            } else {
                const auto it = named_handlers_.find(message.route.name);
                if (it != named_handlers_.end()) {
                    handler = it->second;
                }
            }
        }

        Response response;
        response.request_id = message.request_id;
        if (!handler) {
            response.status = RpcStatus::not_found;
            response.error = "rpc route not found: " + route_key(message.route);
            return response;
        }

        try {
            YUAN_RPC_PROFILE_ZONE("yuan.rpc.local_bus.invoke");
            response = (*handler)(message);
            response.request_id = message.request_id;
            return response;
        } catch (const std::exception &e) {
            response.status = RpcStatus::internal_error;
            response.error = e.what();
            return response;
        } catch (...) {
            response.status = RpcStatus::internal_error;
            response.error = "unknown rpc handler error";
            return response;
        }
    }

    std::size_t LocalBus::size() const
    {
        YUAN_RPC_PROFILE_ZONE("yuan.rpc.local_bus.size");
        std::lock_guard<std::mutex> lock(mutex_);
        return numeric_handlers_.size() + named_handlers_.size();
    }
}
