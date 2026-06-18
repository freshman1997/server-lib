#ifndef YUAN_GAME_SERVER_GLOBAL_GLOBAL_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_GLOBAL_GLOBAL_SERVER_SERVICE_H

#include "application.h"
#include "common/rpc_network.h"
#include "common/service_config.h"
#include "global/rpc/global_msg_echo.h"
#include "global/rpc/global_msg_gm.h"
#include "messaging/tunnel_client_manager.h"

#include <cstdint>
#include <thread>

namespace yuan::game::server
{
    class GlobalServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        explicit GlobalServerService(ServiceServerConfig config);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;

        bool init() override;

        void start() override;

        void stop() override;

        [[nodiscard]] bool ok() const;

    private:
        std::string listen_host_;
        std::uint16_t port_ = 0;
        bool ok_ = false;
        yuan::app::RuntimeContext context_;
        GameServiceId service_id_;
        GlobalMsgEchoContext echo_context_;
        GlobalMsgGmContext gm_context_;
        yuan::rpc::Server global_rpc_;
        mutable TunnelClientManager tunnel_client_manager_;
        rpc_network::RpcNetworkServer rpc_server_;
        bool call_source_zone(const yuan::rpc::Message &message);
    };
}

#endif
