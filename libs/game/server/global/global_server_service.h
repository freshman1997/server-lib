#ifndef YUAN_GAME_SERVER_GLOBAL_GLOBAL_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_GLOBAL_GLOBAL_SERVER_SERVICE_H

#include "application.h"
#include "common/rpc_network.h"
#include "global/global_service.h"
#include "messaging/process_message_manager.h"

#include <cstdint>

namespace yuan::game::server
{
    class GlobalServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        GlobalServerService(GameServiceId service_id, std::uint16_t port, std::uint16_t tunnel_port);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;

        bool init() override;

        void start() override;

        void stop() override;

        [[nodiscard]] bool ok() const;

    private:
        std::uint16_t port_ = 0;
        std::uint16_t tunnel_port_ = 0;
        bool ok_ = false;
        yuan::app::RuntimeContext context_;
        GameServiceId service_id_;
        GlobalService global_;
        mutable ProcessMessageManager messaging_;
        rpc_network::RpcNetworkServer rpc_server_;

        bool register_to_tunnel();

        bool call_source_zone(const yuan::rpc::Message &message);
    };
}

#endif
