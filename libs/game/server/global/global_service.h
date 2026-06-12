#ifndef YUAN_GAME_SERVER_GLOBAL_GLOBAL_SERVICE_H
#define YUAN_GAME_SERVER_GLOBAL_GLOBAL_SERVICE_H

#include "tunnel/tunnel_service.h"

#include <cstdint>
#include <functional>
#include <string>

namespace yuan::game::server
{
    class GlobalService : public ServiceNode
    {
    public:
        explicit GlobalService(ServiceAddress address);

        [[nodiscard]] std::uint64_t request_count() const;

        void set_after_echo(std::function<void(const yuan::rpc::Message &)> callback);

    private:
        std::uint64_t request_count_ = 0;
        std::function<void(const yuan::rpc::Message &)> after_echo_;
    };
}

#endif
