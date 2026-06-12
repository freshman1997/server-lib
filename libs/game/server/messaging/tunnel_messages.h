#ifndef YUAN_GAME_SERVER_MESSAGING_TUNNEL_MESSAGES_H
#define YUAN_GAME_SERVER_MESSAGING_TUNNEL_MESSAGES_H

#include "common/service_node.h"

#include <optional>

namespace yuan::game::server
{
    struct TunnelEnvelope
    {
        std::string source;
        std::string target;
        PackedGameServiceId source_service_id = 0;
        PackedGameServiceId target_service_id = 0;
        GameServiceType target_type = GameServiceType::tunnel;
        enum class ForwardMode : std::uint8_t { specific = 0, random_one = 1, all_of_type = 2 } mode = ForwardMode::specific;
        yuan::rpc::RequestId request_id = 0;
        yuan::rpc::ContinuationId continuation_id = 0;
        yuan::rpc::Route route;
        yuan::rpc::Metadata metadata;
        yuan::rpc::Bytes payload;
    };

    struct TunnelReply
    {
        std::string source;
        std::string target;
        PackedGameServiceId source_service_id = 0;
        PackedGameServiceId target_service_id = 0;
        yuan::rpc::RequestId request_id = 0;
        yuan::rpc::ContinuationId continuation_id = 0;
        yuan::rpc::RpcStatus status = yuan::rpc::RpcStatus::ok;
        std::string error;
        yuan::rpc::Metadata metadata;
        yuan::rpc::Bytes payload;
    };

    struct TunnelRegistration
    {
        PackedGameServiceId service_id = 0;
        std::uint16_t port = 0;
        std::string name;
    };

    bool encode_tunnel_envelope(const TunnelEnvelope &envelope, yuan::rpc::Bytes &out);
    bool encode_tunnel_reply(const TunnelReply &reply, yuan::rpc::Bytes &out);
    bool encode_tunnel_registration(const TunnelRegistration &registration, yuan::rpc::Bytes &out);
    std::optional<TunnelEnvelope> decode_tunnel_envelope(const yuan::rpc::Bytes &in);
    std::optional<TunnelReply> decode_tunnel_reply(const yuan::rpc::Bytes &in);
    std::optional<TunnelRegistration> decode_tunnel_registration(const yuan::rpc::Bytes &in);
}

#endif
