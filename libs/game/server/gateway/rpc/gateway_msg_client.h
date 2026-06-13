#ifndef YUAN_GAME_SERVER_GATEWAY_RPC_GATEWAY_MSG_CLIENT_H
#define YUAN_GAME_SERVER_GATEWAY_RPC_GATEWAY_MSG_CLIENT_H

#include "common/game_messages.h"
#include "common/client_frame.h"
#include "gateway/model/gateway_session_model.h"

#include <functional>

namespace yuan::game::server
{
    struct GatewayMsgContext
    {
        ServiceAddress address;
        std::string public_host;
        std::uint16_t public_port = 0;
        std::function<std::optional<ClientLoginResponse>(ClientLoginRequest)> login_handler;
        std::function<std::optional<ClientGameResponse>(ClientGameRequest, PackedGameServiceId)> game_forward_handler;
        std::function<std::optional<ClientLoginResponse>(ClientLoginRequest, PackedGameServiceId)> logout_handler;
        std::function<bool(ClientPushMessage)> push_handler;
        ClientFrameValidationOptions frame_validation_options;
        ClientFrameReplayGuard frame_replay_guard;
        GatewaySessionModel sessions;
    };

    GatewayInfo gateway_public_info(const GatewayMsgContext &context);
    bool register_gateway_msg_client(yuan::rpc::Server &server, GatewayMsgContext &context);
}

#endif
