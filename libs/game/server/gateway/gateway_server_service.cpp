#include "gateway/gateway_server_service.h"

namespace yuan::game::server
{
    GatewayServerService::GatewayServerService(GameServiceId service_id,
                                                 std::uint16_t port,
                                                 std::uint16_t tunnel_port,
                                                 GameServiceId world_id,
                                                 std::size_t expected_requests)
        : port_(port),
          tunnel_port_(tunnel_port),
          expected_requests_(expected_requests),
          service_id_(service_id),
          world_id_(world_id),
          gateway_({service_id, 500, yuan::game_base::ServerRole::gateway, service_id.world, "gateway"}),
          messaging_({tunnel_port})
    {
        gateway_.set_public_endpoint("127.0.0.1", port_);
    }

    void GatewayServerService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool GatewayServerService::init()
    {
        gateway_.set_login_handler([this](ClientLoginRequest request) {
            return login(request);
        });
        gateway_.set_game_forward_handler([this](ClientGameRequest request, PackedGameServiceId zone_service_id) {
            return forward_game(std::move(request), zone_service_id);
        });
        gateway_.set_logout_handler([this](ClientLoginRequest request, PackedGameServiceId zone_service_id) {
            return logout(request, zone_service_id);
        });
        ok_ = rpc_server_.bind_loopback(port_, gateway_.rpc_server(), expected_requests_);
        return ok_;
    }

    void GatewayServerService::start()
    {
        messaging_.start_heartbeat();
        ok_ = ok_ && register_to_tunnel() && register_to_world() && rpc_server_.run();
    }

    void GatewayServerService::stop()
    {
        messaging_.stop_heartbeat();
        rpc_server_.stop();
    }

    bool GatewayServerService::ok() const
    {
        return ok_;
    }

    bool GatewayServerService::register_to_tunnel()
    {
        TunnelRegistration registration;
        registration.service_id = service_id_.pack();
        registration.port = port_;
        registration.name = "gateway";
        auto response = messaging_.register_service(std::move(registration));
        return response && response->status == yuan::rpc::RpcStatus::ok;
    }

    bool GatewayServerService::register_to_world()
    {
        yuan::rpc::Bytes world_payload;
        if (!encode_gateway_info(gateway_.public_info(), world_payload)) {
            return false;
        }
        auto response = messaging_.send_to_service(service_id_.pack(),
                                                   world_id_.pack(),
                                                   game_route::world_gateway_register(),
                                                   std::move(world_payload));
        return response && response->status == yuan::rpc::RpcStatus::ok;
    }

    std::optional<ClientLoginResponse> GatewayServerService::login(ClientLoginRequest request) const
    {
        yuan::rpc::Bytes query_payload;
        if (!encode_zone_select_request(ZoneSelectRequest{request.player_uid, request.role_id}, query_payload)) {
            return std::nullopt;
        }
        auto response = messaging_.send_to_service(service_id_.pack(),
                                                   world_id_.pack(),
                                                   game_route::world_zone_select(),
                                                   std::move(query_payload));
        if (!response || response->status != yuan::rpc::RpcStatus::ok) {
            return std::nullopt;
        }
        const auto zone = decode_player_zone_update(response->payload);
        if (!zone || zone->zone_service_id == 0) {
            return ClientLoginResponse{false, request.role_id, 0, "world has no zone for role"};
        }

        yuan::rpc::Bytes login_payload;
        if (!encode_client_login_request(request, login_payload)) {
            return std::nullopt;
        }
        auto zone_response = messaging_.send_to_service(service_id_.pack(),
                                                        zone->zone_service_id,
                                                        game_route::zone_player_enter(),
                                                        std::move(login_payload));
        if (!zone_response || zone_response->status != yuan::rpc::RpcStatus::ok) {
            return ClientLoginResponse{false, request.role_id, zone->zone_service_id, "zone login failed"};
        }
        return decode_client_login_response(zone_response->payload);
    }

    std::optional<ClientGameResponse> GatewayServerService::forward_game(ClientGameRequest request, PackedGameServiceId zone_service_id) const
    {
        auto response = messaging_.send_to_service(service_id_.pack(),
                                                   zone_service_id,
                                                   game_route::zone_echo(),
                                                   request.payload);
        if (!response || response->status != yuan::rpc::RpcStatus::ok) {
            return std::nullopt;
        }
        return ClientGameResponse{true, request.role_id, response->payload, "zone game ok"};
    }

    std::optional<ClientLoginResponse> GatewayServerService::logout(ClientLoginRequest request, PackedGameServiceId zone_service_id) const
    {
        yuan::rpc::Bytes logout_payload;
        if (!encode_client_login_request(request, logout_payload)) {
            return std::nullopt;
        }
        auto response = messaging_.send_to_service(service_id_.pack(),
                                                   zone_service_id,
                                                   game_route::zone_player_leave(),
                                                   std::move(logout_payload));
        if (!response || response->status != yuan::rpc::RpcStatus::ok) {
            return std::nullopt;
        }
        return decode_client_login_response(response->payload);
    }
}
