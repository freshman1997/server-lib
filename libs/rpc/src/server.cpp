#include "yuan/rpc/server.h"

#include <utility>

namespace yuan::rpc
{
    bool Server::register_handler(Route route, RequestHandler handler)
    {
        return bus_.bind(std::move(route), std::move(handler));
    }

    void Server::set_dispatcher(RequestHandler dispatcher)
    {
        dispatcher_ = std::move(dispatcher);
    }

    bool Server::unregister_handler(const Route &route)
    {
        return bus_.unbind(route);
    }

    Response Server::handle(const Message &message) const
    {
        if (dispatcher_) return dispatcher_(message);
        return bus_.dispatch(message);
    }

    std::size_t Server::handler_count() const
    {
        return bus_.size();
    }
}
