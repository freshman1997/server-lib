#include "world/model/world_ownership_store.h"

#include "common/game_rpc_protocol.h"
#include "common/proto/world_db_proto.h"
#include "logger.h"

namespace yuan::game::server
{
    std::optional<WorldOwnershipRecord> InMemoryWorldOwnershipStore::get(PlayerId player_id) const
    {
        const auto it = records_.find(player_id);
        if (it == records_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    bool InMemoryWorldOwnershipStore::compare_and_set(PlayerId player_id,
                                                       PackedGameServiceId source_zone_service_id,
                                                       std::uint64_t expected_gateway_session_id,
                                                       WorldOwnershipRecord next)
    {
        const auto current = get(player_id).value_or(WorldOwnershipRecord{});
        if (next.zone_service_id == 0 && source_zone_service_id != 0 && current.zone_service_id != 0 && current.zone_service_id != source_zone_service_id) {
            return false;
        }

        if (next.zone_service_id == 0 && expected_gateway_session_id != 0 && current.gateway_session_id != 0 && current.gateway_session_id != expected_gateway_session_id) {
            return false;
        }

        if (next.zone_service_id == 0) {
            records_.erase(player_id);
        } else {
            records_[player_id] = next;
        }
        
        return true;
    }

    WorldDbProxyOwnershipStore::WorldDbProxyOwnershipStore(PackedGameServiceId source_service_id,
                                                           DbProxyRoutingConfig routing,
                                                           TunnelClientManager &tunnel_client_manager)
        : source_service_id_(source_service_id),
          routing_(std::move(routing)),
          tunnel_client_manager_(&tunnel_client_manager)
    {
    }

    std::optional<WorldOwnershipRecord> WorldDbProxyOwnershipStore::get(PlayerId player_id) const
    {
        (void)player_id;
        return std::nullopt;
    }

    bool WorldDbProxyOwnershipStore::compare_and_set(PlayerId player_id,
                                                     PackedGameServiceId source_zone_service_id,
                                                     std::uint64_t expected_gateway_session_id,
                                                     WorldOwnershipRecord next)
    {
        if (!tunnel_client_manager_ || player_id == 0) {
            return false;
        }
        const auto *target_proxy = select_db_proxy_endpoint(player_id, routing_);
        if (!target_proxy) {
            LOG_ERROR("world ownership compare_and_set failed: world_db_proxy unavailable role_id={}", player_id);
            return false;
        }

        SSWorldDbOwnershipCompareAndSetRequest request;
        request.role_id = player_id;
        request.source_zone_service_id = source_zone_service_id;
        request.expected_gateway_session_id = expected_gateway_session_id;
        request.next_zone_service_id = next.zone_service_id;
        request.next_gateway_session_id = next.gateway_session_id;

        yuan::rpc::Bytes payload;
        if (!encode_binary(request, payload)) {
            return false;
        }
        std::optional<yuan::rpc::Response> response;
        if (!target_proxy->host.empty() && target_proxy->port != 0) {
            yuan::rpc::Message message;
            message.route = game_route::world_db_ownership_compare_and_set();
            message.payload = std::move(payload);
            response = rpc_network::RpcNetworkClient().call(rpc_network::RpcEndpoint{target_proxy->host, target_proxy->port}, message);
        } else {
            response = tunnel_client_manager_->send_to_service(source_service_id_, target_proxy->service_id, game_route::world_db_ownership_compare_and_set(), std::move(payload));
        }
        if (!response || response->status != yuan::rpc::RpcStatus::ok) {
            LOG_ERROR("world ownership compare_and_set via world_db_proxy failed role_id={} proxy_service={} status={}",
                      player_id,
                      target_proxy->service_id,
                      response ? static_cast<int>(response->status) : -1);
            return false;
        }
        const auto body = decode_binary<SSWorldDbOwnershipCompareAndSetResponse>(response->payload);
        if (!body || !body->ok) {
            LOG_ERROR("world ownership compare_and_set decode failed role_id={} proxy_service={}", player_id, target_proxy->service_id);
            return false;
        }
        return body->applied;
    }

}
