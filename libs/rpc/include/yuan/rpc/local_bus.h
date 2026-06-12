#ifndef YUAN_RPC_LOCAL_BUS_H
#define YUAN_RPC_LOCAL_BUS_H

#include "types.h"

#include <mutex>
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
        mutable std::mutex mutex_;
        std::unordered_map<std::string, RequestHandler> handlers_;
    };
}

#endif
