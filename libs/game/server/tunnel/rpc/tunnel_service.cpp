#include "tunnel/rpc/tunnel_service.h"

#include "common/metadata_keys.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace yuan::game::server
{
    namespace
    {
        yuan::rpc::Response handle_tunnel_forward(TunnelService &service, const yuan::rpc::Message &message)
        {
            const auto envelope = decode_tunnel_envelope(message.payload);
            if (!envelope) {
                yuan::rpc::Response response;
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid tunnel envelope";
                return response;
            }

            auto forwarded = *envelope;
            if (forwarded.request_id == 0) {
                forwarded.request_id = message.request_id;
            }

            if (forwarded.continuation_id == 0) {
                forwarded.continuation_id = message.continuation_id();
            }

            auto response = service.forward(std::move(forwarded));
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            return response;
        }

        yuan::rpc::Response handle_tunnel_reply(TunnelService &service, const yuan::rpc::Message &message)
        {
            const auto reply = decode_tunnel_reply(message.payload);
            if (!reply) {
                yuan::rpc::Response response;
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid tunnel reply";
                return response;
            }
            return service.handle_reply(*reply);
        }

        yuan::rpc::Response handle_tunnel_heartbeat(const yuan::rpc::Message &message)
        {
            yuan::rpc::Response response;
            response.status = yuan::rpc::RpcStatus::ok;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            response.metadata[game_metadata_key::tunnel_heartbeat] = "pong";
            return response;
        }

        yuan::rpc::Response handle_registered_local_endpoint(yuan::rpc::Server &server, yuan::rpc::Message message)
        {
            return server.handle(message);
        }
    }

    TunnelService::TunnelService(ServiceAddress address)
        : ServiceNode(std::move(address))
    {
        (void)rpc_server().register_handler(game_route::tunnel_forward(), std::bind_front(handle_tunnel_forward, std::ref(*this)));
        (void)rpc_server().register_handler(game_route::tunnel_reply(), std::bind_front(handle_tunnel_reply, std::ref(*this)));
        (void)rpc_server().register_handler(game_route::tunnel_heartbeat(), handle_tunnel_heartbeat);
    }

    bool TunnelService::register_endpoint(ServiceAddress address, yuan::rpc::Server &server)
    {
        const auto key = service_key(address);
        if (key.empty()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto endpoint_address = address;
        endpoints_[key] = Endpoint{std::move(address), std::bind_front(handle_registered_local_endpoint, std::ref(server))};
        type_index_[static_cast<ServiceTypeId>(endpoint_address.service.type)].push_back(key);

        return true;
    }

    bool TunnelService::register_endpoint_handler(ServiceAddress address, std::function<yuan::rpc::Response(yuan::rpc::Message)> handler)
    {
        const auto key = service_key(address);
        if (key.empty() || !handler) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto endpoint_address = address;
        endpoints_[key] = Endpoint{std::move(address), std::move(handler)};
        type_index_[static_cast<ServiceTypeId>(endpoint_address.service.type)].push_back(key);

        return true;
    }

    bool TunnelService::register_endpoint_handler(PackedGameServiceId service_id, std::function<yuan::rpc::Response(yuan::rpc::Message)> handler)
    {
        if (service_id == 0 || !handler) {
            return false;
        }

        ServiceAddress address;
        address.service = unpack_game_service_id(service_id);
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = std::to_string(service_id);
        auto endpoint_address = address;
        endpoints_[key] = Endpoint{std::move(address), std::move(handler)};
        type_index_[static_cast<ServiceTypeId>(endpoint_address.service.type)].push_back(key);

        return true;
    }

    bool TunnelService::unregister_endpoint(const ServiceAddress &address)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return endpoints_.erase(service_key(address)) != 0;
    }

    yuan::rpc::Response TunnelService::forward(TunnelEnvelope envelope)
    {
        if (envelope.mode == TunnelEnvelope::ForwardMode::all_of_type) {
            return forward_all_of_type(std::move(envelope));
        }

        const auto selected = select_endpoint(envelope);
        if (!selected) {
            yuan::rpc::Response response;
            response.status = yuan::rpc::RpcStatus::not_found;
            response.error = "tunnel target not found";
            return response;
        }

        auto message = make_forward_message(envelope);
        auto response = selected->handler(std::move(message));
        response.metadata[game_metadata_key::tunnel_instance] = service_key(address());
        return response;
    }

    yuan::rpc::Response TunnelService::forward_all_of_type(TunnelEnvelope envelope)
    {
        std::vector<Endpoint> endpoints;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto type_it = type_index_.find(static_cast<ServiceTypeId>(envelope.target_type));
            if (type_it != type_index_.end()) {
                for (const auto &key : type_it->second) {
                    const auto endpoint_it = endpoints_.find(key);
                    if (endpoint_it != endpoints_.end()) {
                        endpoints.push_back(endpoint_it->second);
                    }
                }
            }
        }
        if (endpoints.empty()) {
            yuan::rpc::Response response;
            response.status = yuan::rpc::RpcStatus::not_found;
            response.error = "tunnel target type not found";
            return response;
        }

        std::size_t ok_count = 0;
        yuan::rpc::Response last_response;
        for (auto &endpoint : endpoints) {
            auto message = make_forward_message(envelope);
            auto response = endpoint.handler(std::move(message));
            if (response.status == yuan::rpc::RpcStatus::ok) {
                ++ok_count;
            }
            last_response = std::move(response);
        }

        last_response.status = ok_count == endpoints.size() ? yuan::rpc::RpcStatus::ok : yuan::rpc::RpcStatus::internal_error;
        last_response.metadata[game_metadata_key::tunnel_broadcast_count] = std::to_string(endpoints.size());
        last_response.metadata[game_metadata_key::tunnel_broadcast_ok] = std::to_string(ok_count);
        last_response.metadata[game_metadata_key::tunnel_instance] = service_key(address());
        return last_response;
    }

    yuan::rpc::Message TunnelService::make_forward_message(const TunnelEnvelope &envelope) const
    {
        yuan::rpc::Message message;
        message.kind = yuan::rpc::MessageKind::request;
        message.request_id = envelope.request_id;
        message.set_continuation_id(envelope.continuation_id);
        message.route = envelope.route;
        message.metadata = envelope.metadata;
        message.metadata[game_metadata_key::tunnel_source] = envelope.source;
        message.metadata[game_metadata_key::tunnel_target] = envelope.target;
        message.metadata[game_metadata_key::tunnel_source_service_id] = std::to_string(envelope.source_service_id);
        message.metadata[game_metadata_key::tunnel_target_service_id] = std::to_string(envelope.target_service_id);
        message.metadata[game_metadata_key::tunnel_origin_request_id] = std::to_string(envelope.request_id);
        message.metadata[game_metadata_key::tunnel_origin_continuation_id] = std::to_string(envelope.continuation_id);
        message.payload = envelope.payload;
        return message;
    }

    bool TunnelService::forward_async(TunnelEnvelope envelope, ReplyHandler on_reply)
    {
        if (!on_reply || envelope.request_id == 0 || envelope.continuation_id == 0) {
            return false;
        }

        const auto key = pending_key(envelope.request_id, envelope.continuation_id);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_replies_[key] = PendingReply{envelope.source, std::move(on_reply)};
        }

        auto response = forward(std::move(envelope));
        if (response.status == yuan::rpc::RpcStatus::ok) {
            return true;
        }

        ReplyHandler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pending_replies_.find(key);
            if (it != pending_replies_.end()) {
                handler = std::move(it->second.handler);
                pending_replies_.erase(it);
            }
        }

        if (handler) {
            handler(std::move(response));
        }

        return false;
    }

    yuan::rpc::Response TunnelService::handle_reply(TunnelReply reply)
    {
        ReplyHandler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pending_replies_.find(pending_key(reply.request_id, reply.continuation_id));
            if (it == pending_replies_.end()) {
                yuan::rpc::Response response;
                response.status = yuan::rpc::RpcStatus::not_found;
                response.error = "tunnel pending reply not found";
                return response;
            }
            handler = std::move(it->second.handler);
            pending_replies_.erase(it);
        }

        yuan::rpc::Response forwarded;
        forwarded.request_id = reply.request_id;
        forwarded.set_continuation_id(reply.continuation_id);
        forwarded.status = reply.status;
        forwarded.error = std::move(reply.error);
        forwarded.metadata = std::move(reply.metadata);
        forwarded.metadata[game_metadata_key::tunnel_reply_source] = std::move(reply.source);
        forwarded.metadata[game_metadata_key::tunnel_reply_target] = std::move(reply.target);
        forwarded.metadata[game_metadata_key::tunnel_reply_source_service_id] = std::to_string(reply.source_service_id);
        forwarded.metadata[game_metadata_key::tunnel_reply_target_service_id] = std::to_string(reply.target_service_id);
        forwarded.payload = std::move(reply.payload);
        handler(forwarded);

        yuan::rpc::Response ack;
        ack.status = yuan::rpc::RpcStatus::ok;
        ack.request_id = reply.request_id;
        ack.set_continuation_id(reply.continuation_id);
        return ack;
    }

    std::size_t TunnelService::endpoint_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return endpoints_.size();
    }

    std::size_t TunnelService::pending_reply_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_replies_.size();
    }

    std::optional<TunnelService::Endpoint> TunnelService::select_endpoint(const TunnelEnvelope &envelope)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (envelope.mode == TunnelEnvelope::ForwardMode::random_one) {
            const auto type_it = type_index_.find(static_cast<ServiceTypeId>(envelope.target_type));
            if (type_it == type_index_.end() || type_it->second.empty()) {
                return std::nullopt;
            }

            std::uniform_int_distribution<std::size_t> dist(0, type_it->second.size() - 1);
            for (std::size_t tries = 0; tries < type_it->second.size(); ++tries) {
                const auto &key = type_it->second[dist(random_)];
                const auto it = endpoints_.find(key);
                if (it != endpoints_.end()) {
                    return it->second;
                }
            }

            return std::nullopt;
        }

        const auto target_key = tunnel_route_key(envelope.target_service_id, envelope.target);
        const auto it = endpoints_.find(target_key);
        if (it == endpoints_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::string TunnelService::pending_key(yuan::rpc::RequestId request_id, yuan::rpc::ContinuationId continuation_id)
    {
        return std::to_string(request_id) + ":" + std::to_string(continuation_id);
    }

    std::string TunnelService::tunnel_route_key(PackedGameServiceId service_id, const std::string &fallback)
    {
        if (service_id != 0) {
            return std::to_string(service_id);
        }
        return fallback;
    }

    bool TunnelCluster::add(std::shared_ptr<TunnelService> tunnel)
    {
        if (!tunnel) {
            return false;
        }
        tunnels_.push_back(std::move(tunnel));
        return true;
    }

    bool TunnelCluster::register_endpoint(ServiceAddress address, yuan::rpc::Server &server)
    {
        bool ok = false;
        for (auto &tunnel : tunnels_) {
            ok = tunnel->register_endpoint(address, server) || ok;
        }
        return ok;
    }

    yuan::rpc::Response TunnelCluster::forward(TunnelEnvelope envelope)
    {
        if (tunnels_.empty()) {
            yuan::rpc::Response response;
            response.status = yuan::rpc::RpcStatus::unavailable;
            response.error = "no tunnel instance available";
            return response;
        }
        
        const std::size_t index = next_++ % tunnels_.size();
        return tunnels_[index]->forward(std::move(envelope));
    }

    std::size_t TunnelCluster::size() const
    {
        return tunnels_.size();
    }
}
