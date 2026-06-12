#ifndef YUAN_GAME_SERVER_GATEWAY_GATEWAY_SERVICE_H
#define YUAN_GAME_SERVER_GATEWAY_GATEWAY_SERVICE_H

#include "common/game_messages.h"

#include <functional>
#include <unordered_map>

namespace yuan::game::server
{
    class GatewayService : public ServiceNode
    {
    public:
        using LoginHandler = std::function<std::optional<ClientLoginResponse>(ClientLoginRequest)>;
        using GameForwardHandler = std::function<std::optional<ClientGameResponse>(ClientGameRequest, PackedGameServiceId)>;
        using LogoutHandler = std::function<std::optional<ClientLoginResponse>(ClientLoginRequest, PackedGameServiceId)>;

        explicit GatewayService(ServiceAddress address);

        void set_login_handler(LoginHandler handler);
        void set_game_forward_handler(GameForwardHandler handler);
        void set_logout_handler(LogoutHandler handler);

        [[nodiscard]] GatewayInfo public_info() const;

        void set_public_endpoint(std::string host, std::uint16_t port);

    private:
        std::string public_host_ = "127.0.0.1";
        std::uint16_t public_port_ = 0;
        LoginHandler login_handler_;
        GameForwardHandler game_forward_handler_;
        LogoutHandler logout_handler_;
        std::unordered_map<RoleId, PackedGameServiceId> zone_by_role_;
    };
}

#endif
