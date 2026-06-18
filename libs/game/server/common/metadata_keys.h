#ifndef YUAN_GAME_SERVER_COMMON_METADATA_KEYS_H
#define YUAN_GAME_SERVER_COMMON_METADATA_KEYS_H

namespace yuan::game::server::game_metadata_key
{
    inline constexpr const char *rpc_connection_id = "rpc.connection_id";
    inline constexpr const char *rpc_close_connection = "rpc.close_connection";
    inline constexpr const char *rpc_defer_response = "rpc.defer_response";

    inline constexpr const char *gateway_session_id = "gateway.session_id";
    inline constexpr const char *gateway_zone_service_id = "gateway.zone_service_id";
    inline constexpr const char *gateway_internal_secret = "gateway.internal_secret";

    inline constexpr const char *tunnel_source = "tunnel.source";
    inline constexpr const char *tunnel_target = "tunnel.target";
    inline constexpr const char *tunnel_source_service_id = "tunnel.source_service_id";
    inline constexpr const char *tunnel_target_service_id = "tunnel.target_service_id";
    inline constexpr const char *tunnel_origin_request_id = "tunnel.origin.request_id";
    inline constexpr const char *tunnel_origin_continuation_id = "tunnel.origin.continuation_id";
    inline constexpr const char *tunnel_reply_source = "tunnel.reply.source";
    inline constexpr const char *tunnel_reply_target = "tunnel.reply.target";
    inline constexpr const char *tunnel_reply_source_service_id = "tunnel.reply.source_service_id";
    inline constexpr const char *tunnel_reply_target_service_id = "tunnel.reply.target_service_id";
    inline constexpr const char *tunnel_heartbeat = "tunnel.heartbeat";
    inline constexpr const char *tunnel_instance = "tunnel.instance";
    inline constexpr const char *tunnel_broadcast_count = "tunnel.broadcast.count";
    inline constexpr const char *tunnel_broadcast_ok = "tunnel.broadcast.ok";

    inline constexpr const char *global_node = "global.node";
    inline constexpr const char *zone_node = "zone.node";
}

#endif
