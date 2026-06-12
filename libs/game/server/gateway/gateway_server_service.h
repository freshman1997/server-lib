#ifndef YUAN_GAME_SERVER_GATEWAY_GATEWAY_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_GATEWAY_GATEWAY_SERVER_SERVICE_H

#include "application.h"
#include "common/rpc_network.h"
#include "gateway/gateway_service.h"
#include "messaging/process_message_manager.h"

namespace yuan::game::server
{
    class GatewayServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        GatewayServerService(GameServiceId service_id,
                              std::uint16_t port,
                              std::uint16_t tunnel_port,
                              GameServiceId world_id,
                              std::size_t expected_requests = 1);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;
        bool init() override;
        void start() override;
        void stop() override;

        [[nodiscard]] bool ok() const;

    private:
        bool register_to_tunnel();
        bool register_to_world();
        std::optional<ClientLoginResponse> login(ClientLoginRequest request) const;
        std::optional<ClientLoginResponse> logout(ClientLoginRequest request, PackedGameServiceId zone_service_id) const;
        std::optional<ClientGameResponse> forward_game(ClientGameRequest request, PackedGameServiceId zone_service_id) const;

        std::uint16_t port_ = 0;
        std::uint16_t tunnel_port_ = 0;
        std::size_t expected_requests_ = 1;
        bool ok_ = false;
        yuan::app::RuntimeContext context_;
        GameServiceId service_id_;
        GameServiceId world_id_;
        GatewayService gateway_;
        mutable ProcessMessageManager messaging_;
        rpc_network::RpcNetworkServer rpc_server_;
    };
}

#endif
