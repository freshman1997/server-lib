#ifndef YUAN_GAME_SERVER_COMMON_SERVICE_NODE_H
#define YUAN_GAME_SERVER_COMMON_SERVICE_NODE_H

#include "common/service_id.h"
#include "game_base/types.h"
#include "yuan/rpc/rpc.h"

#include <memory>
#include <string>
#include <utility>

namespace yuan::game::server
{
    using ServiceNodeId = yuan::game_base::NodeId;

    struct ServiceAddress
    {
        GameServiceId service;
        ServiceNodeId id = 0;
        yuan::game_base::ServerRole role = yuan::game_base::ServerRole::gateway;
        yuan::game_base::ShardId shard = 0;
        std::string name;
    };

    inline std::string service_key(const ServiceAddress &address)
    {
        if (address.service.instance != 0) {
            return service_id_key(address.service);
        }
        if (!address.name.empty()) {
            return address.name;
        }
        return std::string(yuan::game_base::to_string(address.role)) + ":" + std::to_string(address.shard) + ":" + std::to_string(address.id);
    }

    class ServiceNode
    {
    public:
        explicit ServiceNode(ServiceAddress address)
            : address_(std::move(address))
        {
        }

        virtual ~ServiceNode() = default;

        [[nodiscard]] const ServiceAddress &address() const
        {
            return address_;
        }

        [[nodiscard]] yuan::rpc::Server &rpc_server()
        {
            return rpc_server_;
        }

        [[nodiscard]] const yuan::rpc::Server &rpc_server() const
        {
            return rpc_server_;
        }

    private:
        ServiceAddress address_;
        yuan::rpc::Server rpc_server_;
    };
}

#endif
