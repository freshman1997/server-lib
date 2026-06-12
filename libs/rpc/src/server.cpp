#include "yuan/rpc/server.h"

#include <utility>

namespace yuan::rpc
{
    bool Server::register_handler(Route route, RequestHandler handler)
    {
        return bus_.bind(std::move(route), std::move(handler));
    }

    bool Server::unregister_handler(const Route &route)
    {
        return bus_.unbind(route);
    }

    Response Server::handle(const Message &message) const
    {
        return bus_.dispatch(message);
    }

    std::size_t Server::handler_count() const
    {
        return bus_.size();
    }
}
