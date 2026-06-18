#ifndef YUAN_GAME_SERVER_COMMON_GAME_RPC_PROTOCOL_H
#define YUAN_GAME_SERVER_COMMON_GAME_RPC_PROTOCOL_H

#include "yuan/rpc/types.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace yuan::game::server
{
    enum class GameRpcService : std::uint32_t
    {
        tunnel = 1,
        global = 2,
        zone = 3,
        world = 4,
        gateway = 5,
        web = 6,
        rank = 7,
        player_db = 8,
        world_db = 9,
        global_db = 10
    };

    enum class GameRpcMethod : std::uint32_t
    {
        tunnel_forward = 1,
        tunnel_reply = 2,
        tunnel_register = 3,
        tunnel_heartbeat = 4,

        global_echo = 100,
        global_player_lookup = 101,
        global_gm_execute = 102,

        zone_echo = 200,
        zone_player_enter = 201,
        zone_player_leave = 202,
        zone_gm_execute = 203,
        zone_time_sync = 204,

        world_login_options = 300,
        world_gateway_register = 301,
        world_zone_register = 302,
        world_zone_select = 303,
        world_player_zone_get = 304,
        world_player_zone_set = 305,
        world_gm_forward = 306,

        gateway_login = 400,
        gateway_game_forward = 401,
        gateway_logout = 402,
        gateway_time_sync = 403,
        gateway_push = 404,
        gateway_session_close = 405,

        rank_role_update = 500,
        rank_role_get = 501,
        rank_score_update = 502,
        rank_score_remove = 503,
        rank_score_get = 504,
        rank_top_get = 505,

        player_db_load_role = 600,
        player_db_save_role = 601,

        world_db_role_location_get = 700,
        world_db_role_location_set = 701,

        global_db_config_get = 800,
        global_db_config_set = 801
    };

    inline yuan::rpc::Route make_game_route(GameRpcService service, GameRpcMethod method, std::string_view debug_name = {})
    {
        yuan::rpc::Route route;
        route.service = static_cast<std::uint32_t>(service);
        route.method = static_cast<std::uint32_t>(method);
        route.name = std::string(debug_name);
        return route;
    }

    namespace game_route
    {
        inline yuan::rpc::Route tunnel_forward()
        {
            return make_game_route(GameRpcService::tunnel, GameRpcMethod::tunnel_forward, "tunnel.forward");
        }

        inline yuan::rpc::Route tunnel_reply()
        {
            return make_game_route(GameRpcService::tunnel, GameRpcMethod::tunnel_reply, "tunnel.reply");
        }

        inline yuan::rpc::Route tunnel_register()
        {
            return make_game_route(GameRpcService::tunnel, GameRpcMethod::tunnel_register, "tunnel.register");
        }

        inline yuan::rpc::Route tunnel_heartbeat()
        {
            return make_game_route(GameRpcService::tunnel, GameRpcMethod::tunnel_heartbeat, "tunnel.heartbeat");
        }

        inline yuan::rpc::Route global_echo()
        {
            return make_game_route(GameRpcService::global, GameRpcMethod::global_echo, "global.echo");
        }

        inline yuan::rpc::Route global_gm_execute()
        {
            return make_game_route(GameRpcService::global, GameRpcMethod::global_gm_execute, "global.gm.execute");
        }

        inline yuan::rpc::Route zone_echo()
        {
            return make_game_route(GameRpcService::zone, GameRpcMethod::zone_echo, "zone.echo");
        }

        inline yuan::rpc::Route zone_player_enter()
        {
            return make_game_route(GameRpcService::zone, GameRpcMethod::zone_player_enter, "zone.player.enter");
        }

        inline yuan::rpc::Route zone_player_leave()
        {
            return make_game_route(GameRpcService::zone, GameRpcMethod::zone_player_leave, "zone.player.leave");
        }

        inline yuan::rpc::Route zone_gm_execute()
        {
            return make_game_route(GameRpcService::zone, GameRpcMethod::zone_gm_execute, "zone.gm.execute");
        }

        inline yuan::rpc::Route zone_time_sync()
        {
            return make_game_route(GameRpcService::zone, GameRpcMethod::zone_time_sync, "zone.time_sync");
        }

        inline yuan::rpc::Route world_login_options()
        {
            return make_game_route(GameRpcService::world, GameRpcMethod::world_login_options, "world.login.options");
        }

        inline yuan::rpc::Route world_gateway_register()
        {
            return make_game_route(GameRpcService::world, GameRpcMethod::world_gateway_register, "world.gateway.register");
        }

        inline yuan::rpc::Route world_zone_register()
        {
            return make_game_route(GameRpcService::world, GameRpcMethod::world_zone_register, "world.zone.register");
        }

        inline yuan::rpc::Route world_zone_select()
        {
            return make_game_route(GameRpcService::world, GameRpcMethod::world_zone_select, "world.zone.select");
        }

        inline yuan::rpc::Route world_player_zone_get()
        {
            return make_game_route(GameRpcService::world, GameRpcMethod::world_player_zone_get, "world.player.zone.get");
        }

        inline yuan::rpc::Route world_player_zone_set()
        {
            return make_game_route(GameRpcService::world, GameRpcMethod::world_player_zone_set, "world.player.zone.set");
        }

        inline yuan::rpc::Route world_gm_forward()
        {
            return make_game_route(GameRpcService::world, GameRpcMethod::world_gm_forward, "world.gm.forward");
        }

        inline yuan::rpc::Route gateway_login()
        {
            return make_game_route(GameRpcService::gateway, GameRpcMethod::gateway_login, "gateway.login");
        }

        inline yuan::rpc::Route gateway_game_forward()
        {
            return make_game_route(GameRpcService::gateway, GameRpcMethod::gateway_game_forward, "gateway.game.forward");
        }

        inline yuan::rpc::Route gateway_logout()
        {
            return make_game_route(GameRpcService::gateway, GameRpcMethod::gateway_logout, "gateway.logout");
        }

        inline yuan::rpc::Route gateway_time_sync()
        {
            return make_game_route(GameRpcService::gateway, GameRpcMethod::gateway_time_sync, "gateway.time_sync");
        }

        inline yuan::rpc::Route gateway_push()
        {
            return make_game_route(GameRpcService::gateway, GameRpcMethod::gateway_push, "gateway.push");
        }

        inline yuan::rpc::Route gateway_session_close()
        {
            return make_game_route(GameRpcService::gateway, GameRpcMethod::gateway_session_close, "gateway.session.close");
        }

        inline yuan::rpc::Route rank_role_update()
        {
            return make_game_route(GameRpcService::rank, GameRpcMethod::rank_role_update, "rank.role.update");
        }

        inline yuan::rpc::Route rank_role_get()
        {
            return make_game_route(GameRpcService::rank, GameRpcMethod::rank_role_get, "rank.role.get");
        }

        inline yuan::rpc::Route rank_score_update()
        {
            return make_game_route(GameRpcService::rank, GameRpcMethod::rank_score_update, "rank.score.update");
        }

        inline yuan::rpc::Route rank_score_remove()
        {
            return make_game_route(GameRpcService::rank, GameRpcMethod::rank_score_remove, "rank.score.remove");
        }

        inline yuan::rpc::Route rank_score_get()
        {
            return make_game_route(GameRpcService::rank, GameRpcMethod::rank_score_get, "rank.score.get");
        }

        inline yuan::rpc::Route rank_top_get()
        {
            return make_game_route(GameRpcService::rank, GameRpcMethod::rank_top_get, "rank.top.get");
        }

        inline yuan::rpc::Route player_db_load_role()
        {
            return make_game_route(GameRpcService::player_db, GameRpcMethod::player_db_load_role, "player_db.load_role");
        }

        inline yuan::rpc::Route player_db_save_role()
        {
            return make_game_route(GameRpcService::player_db, GameRpcMethod::player_db_save_role, "player_db.save_role");
        }

        inline yuan::rpc::Route world_db_role_location_get()
        {
            return make_game_route(GameRpcService::world_db, GameRpcMethod::world_db_role_location_get, "world_db.role_location.get");
        }

        inline yuan::rpc::Route world_db_role_location_set()
        {
            return make_game_route(GameRpcService::world_db, GameRpcMethod::world_db_role_location_set, "world_db.role_location.set");
        }

        inline yuan::rpc::Route global_db_config_get()
        {
            return make_game_route(GameRpcService::global_db, GameRpcMethod::global_db_config_get, "global_db.config.get");
        }

        inline yuan::rpc::Route global_db_config_set()
        {
            return make_game_route(GameRpcService::global_db, GameRpcMethod::global_db_config_set, "global_db.config.set");
        }

    }
}

#endif
