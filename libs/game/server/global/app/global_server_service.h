#ifndef YUAN_GAME_SERVER_GLOBAL_GLOBAL_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_GLOBAL_GLOBAL_SERVER_SERVICE_H

#include "application.h"
#include "common/rpc_network.h"
#include "global/rpc/global_msg_echo.h"
#include "global/rpc/global_msg_gm.h"
#include "messaging/process_message_manager.h"

#include <cstdint>
#include <thread>

namespace yuan::game::server
{
    class GlobalServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        GlobalServerService(GameServiceId service_id,
                            std::string listen_host,
                            std::uint16_t port,
                            std::vector<rpc_network::RpcEndpoint> tunnel_endpoints,
                            std::uint64_t tunnel_heartbeat_interval_ms);

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
        mutable ProcessMessageManager messaging_;
        rpc_network::RpcNetworkServer rpc_server_;
        std::jthread register_thread_;

        bool register_to_tunnel();
        void register_loop(std::stop_token stop_token);

        bool call_source_zone(const yuan::rpc::Message &message);
    };
}

#endif
