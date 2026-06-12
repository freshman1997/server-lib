#include "yuan/rpc/local_bus.h"

#include <exception>
#include <utility>

namespace yuan::rpc
{
    bool LocalBus::bind(Route route, RequestHandler handler)
    {
        if (!route.valid() || !handler) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        return handlers_.emplace(route_key(route), std::move(handler)).second;
    }

    bool LocalBus::unbind(const Route &route)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return handlers_.erase(route_key(route)) != 0;
    }

    Response LocalBus::dispatch(const Message &message) const
    {
        RequestHandler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = handlers_.find(route_key(message.route));
            if (it != handlers_.end()) {
                handler = it->second;
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
            response = handler(message);
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
        std::lock_guard<std::mutex> lock(mutex_);
        return handlers_.size();
    }
}
