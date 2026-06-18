#ifndef YUAN_GAME_SERVER_GATEWAY_RPC_GATEWAY_MSG_CLIENT_H
#define YUAN_GAME_SERVER_GATEWAY_RPC_GATEWAY_MSG_CLIENT_H

#include "common/codec/game_binary_codec.h"
#include "gateway/model/gateway_session_model.h"

#include <functional>

namespace yuan::game::server
{
    struct GatewayForwardContext
    {
        std::uint64_t gateway_session_id = 0;
        std::uint64_t connection_id = 0;
    };

    using GatewayZoneSelector = std::function<PackedGameServiceId()>;
    using GatewayZoneForwarder = std::function<std::optional<yuan::rpc::Response>(PackedGameServiceId, yuan::rpc::Route, GatewayForwardContext, yuan::rpc::Bytes)>;
    using GatewayClientPusher = std::function<bool(std::uint64_t, yuan::rpc::Bytes)>;
    using GatewaySessionCloser = std::function<bool(std::uint64_t)>;

    struct GatewayMsgContext
    {
        ServiceAddress address;
        std::string public_host;
        std::uint16_t public_port = 0;
        std::uint64_t login_token_secret = kDefaultLoginTokenSecret;
        std::uint64_t gateway_internal_secret = kDefaultLoginTokenSecret;
        GatewaySessionModel sessions;
    };

    SSGatewayInfo gateway_public_info(const GatewayMsgContext &context);
    bool register_gateway_msg_client(yuan::rpc::Server &server, GatewayMsgContext &context, GatewayZoneSelector select_login_zone, GatewayZoneForwarder forward_to_zone, GatewayClientPusher push_to_client, GatewaySessionCloser close_session = {});
}

#endif
