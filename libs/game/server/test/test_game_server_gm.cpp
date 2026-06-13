#include "game_server.h"

#include "base/time.h"

#include <cstdlib>
#include <iostream>
#include <memory>

namespace
{
    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }
}

int main()
{
    using namespace yuan::game::server;

    yuan::base::time::set_system_time_offset_seconds(0);

    auto tunnel = std::make_shared<TunnelService>(ServiceAddress{{1, 1, GameServiceType::tunnel, 1, 1}, 1, yuan::game_base::ServerRole::gateway, 1, "tunnel"});
    TunnelCluster tunnels;
    if (!require(tunnels.add(tunnel), "tunnel should be added")) {
        return 1;
    }

    const ServiceAddress global_address{{1, 1, GameServiceType::global, 1, 1}, 100, yuan::game_base::ServerRole::world, 1, "global"};
    GlobalMsgEchoContext global_echo{global_address};
    GlobalMsgGmContext global_gm;
    yuan::rpc::Server global_rpc;
    register_global_builtin_gm(global_gm);
    if (!require(register_global_msg_echo(global_rpc, global_echo) && register_global_msg_gm(global_rpc, global_gm),
                 "global handlers should register")) {
        return 8;
    }
    WorldMsgContext world{ServiceAddress{{1, 1, GameServiceType::world, 1, 1}, 400, yuan::game_base::ServerRole::world, 1, "world"}};
    yuan::rpc::Server world_rpc;
    world.gm_forward_handler = [&](GmCommandRequest request) -> std::optional<GmCommandResponse> {
        const auto definition = GmCommandRegistry::instance().find(request.command);
        if (!definition) {
            return GmCommandResponse{false, "unknown gm command: " + request.command};
        }
        if (request.target_service_id == 0) {
            request.target_service_id = pack_game_service_id(1, 1, definition->executor_type, 1);
        }

        const auto route = gm_execute_route_for(definition->executor_type);
        if (!route) {
            return GmCommandResponse{false, "gm executor type is not routable"};
        }

        yuan::rpc::Bytes gm_payload;
        if (!encode_gm_command_request(request, gm_payload)) {
            return GmCommandResponse{false, "failed to encode gm command"};
        }
        TunnelEnvelope envelope;
        envelope.source_service_id = world.address.service.pack();
        envelope.target_service_id = request.target_service_id;
        envelope.source = service_key(world.address);
        envelope.target = std::to_string(request.target_service_id);
        envelope.route = *route;
        envelope.payload = std::move(gm_payload);
        auto response = tunnels.forward(std::move(envelope));
        if (response.status != yuan::rpc::RpcStatus::ok) {
            return GmCommandResponse{false, response.error.empty() ? "gm target failed" : response.error};
        }
        return decode_gm_command_response(response.payload);
    };
    if (!require(register_world_msg(world_rpc, world), "world handlers should register")) {
        return 9;
    }

    if (!require(tunnels.register_endpoint(global_address, global_rpc), "global should register on tunnel")) {
        return 2;
    }
    if (!require(tunnels.register_endpoint(world.address, world_rpc), "world should register on tunnel")) {
        return 3;
    }

    yuan::rpc::Bytes payload;
    if (!require(encode_gm_command_request(GmCommandRequest{0, "set_time_offset_seconds", {"123"}}, payload),
                 "gm request should encode")) {
        return 4;
    }

    yuan::rpc::Message message;
    message.route = game_route::world_gm_forward();
    message.payload = std::move(payload);
    const auto response = world_rpc.handle(message);
    if (!require(response.status == yuan::rpc::RpcStatus::ok, "world gm forward should succeed")) {
        return 5;
    }

    const auto gm_response = decode_gm_command_response(response.payload);
    if (!require(gm_response && gm_response->ok, "gm response should be ok")) {
        return 6;
    }
    if (!require(yuan::base::time::system_time_offset_seconds() == 123, "time offset should be updated by gm")) {
        return 7;
    }

    yuan::base::time::set_system_time_offset_seconds(0);
    return EXIT_SUCCESS;
}
