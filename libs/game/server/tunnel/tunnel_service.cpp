#include "tunnel/tunnel_service.h"

#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace yuan::game::server
{
    namespace
    {
        void append_u32(yuan::rpc::Bytes &out, std::uint32_t value)
        {
            out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
            out.push_back(static_cast<std::uint8_t>(value & 0xff));
        }

        void append_u64(yuan::rpc::Bytes &out, std::uint64_t value)
        {
            out.push_back(static_cast<std::uint8_t>((value >> 56) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 48) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 40) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 32) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
            out.push_back(static_cast<std::uint8_t>(value & 0xff));
        }

        bool read_u32(const yuan::rpc::Bytes &in, std::size_t &offset, std::uint32_t &value)
        {
            if (in.size() - offset < sizeof(std::uint32_t)) {
                return false;
            }
            value = (static_cast<std::uint32_t>(in[offset]) << 24) |
                    (static_cast<std::uint32_t>(in[offset + 1]) << 16) |
                    (static_cast<std::uint32_t>(in[offset + 2]) << 8) |
                    static_cast<std::uint32_t>(in[offset + 3]);
            offset += sizeof(std::uint32_t);
            return true;
        }

        bool read_u64(const yuan::rpc::Bytes &in, std::size_t &offset, std::uint64_t &value)
        {
            if (in.size() - offset < sizeof(std::uint64_t)) {
                return false;
            }
            value = (static_cast<std::uint64_t>(in[offset]) << 56) |
                    (static_cast<std::uint64_t>(in[offset + 1]) << 48) |
                    (static_cast<std::uint64_t>(in[offset + 2]) << 40) |
                    (static_cast<std::uint64_t>(in[offset + 3]) << 32) |
                    (static_cast<std::uint64_t>(in[offset + 4]) << 24) |
                    (static_cast<std::uint64_t>(in[offset + 5]) << 16) |
                    (static_cast<std::uint64_t>(in[offset + 6]) << 8) |
                    static_cast<std::uint64_t>(in[offset + 7]);
            offset += sizeof(std::uint64_t);
            return true;
        }

        bool append_bytes(yuan::rpc::Bytes &out, const yuan::rpc::Bytes &value)
        {
            if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }
            append_u32(out, static_cast<std::uint32_t>(value.size()));
            out.insert(out.end(), value.begin(), value.end());
            return true;
        }

        void append_raw(yuan::rpc::Bytes &out, const void *data, std::size_t size)
        {
            if (size == 0) {
                return;
            }
            const auto offset = out.size();
            out.resize(offset + size);
            std::memcpy(out.data() + offset, data, size);
        }

        bool append_string(yuan::rpc::Bytes &out, const std::string &value)
        {
            if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }
            append_u32(out, static_cast<std::uint32_t>(value.size()));
            append_raw(out, value.data(), value.size());
            return true;
        }

        bool read_bytes(const yuan::rpc::Bytes &in, std::size_t &offset, yuan::rpc::Bytes &value)
        {
            std::uint32_t size = 0;
            if (!read_u32(in, offset, size) || in.size() - offset < size) {
                return false;
            }
            value.assign(in.begin() + static_cast<std::ptrdiff_t>(offset), in.begin() + static_cast<std::ptrdiff_t>(offset + size));
            offset += size;
            return true;
        }

        bool read_string(const yuan::rpc::Bytes &in, std::size_t &offset, std::string &value)
        {
            std::uint32_t size = 0;
            if (!read_u32(in, offset, size) || in.size() - offset < size) {
                return false;
            }
            value.assign(reinterpret_cast<const char *>(in.data() + offset), size);
            offset += size;
            return true;
        }
    }

    bool encode_tunnel_envelope(const TunnelEnvelope &envelope, yuan::rpc::Bytes &out)
    {
        constexpr std::uint32_t envelope_version = 1;
        out.clear();
        append_u32(out, envelope_version);
        if (!append_string(out, envelope.source) || !append_string(out, envelope.target)) {
            return false;
        }
        append_u64(out, envelope.source_service_id);
        append_u64(out, envelope.target_service_id);
        append_u32(out, static_cast<std::uint32_t>(envelope.target_type));
        append_u32(out, static_cast<std::uint32_t>(envelope.mode));
        append_u64(out, envelope.request_id);
        append_u64(out, envelope.continuation_id);
        append_u32(out, envelope.route.service);
        append_u32(out, envelope.route.method);
        if (!append_string(out, envelope.route.name)) {
            return false;
        }
        if (envelope.metadata.size() > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        append_u32(out, static_cast<std::uint32_t>(envelope.metadata.size()));
        for (const auto &[key, value] : envelope.metadata) {
            if (!append_string(out, key) || !append_string(out, value)) {
                return false;
            }
        }
        return append_bytes(out, envelope.payload);
    }

    bool encode_tunnel_reply(const TunnelReply &reply, yuan::rpc::Bytes &out)
    {
        constexpr std::uint32_t reply_version = 1;
        out.clear();
        append_u32(out, reply_version);
        if (!append_string(out, reply.source) || !append_string(out, reply.target)) {
            return false;
        }
        append_u64(out, reply.source_service_id);
        append_u64(out, reply.target_service_id);
        append_u64(out, reply.request_id);
        append_u64(out, reply.continuation_id);
        append_u32(out, static_cast<std::uint32_t>(reply.status));
        if (!append_string(out, reply.error)) {
            return false;
        }
        if (reply.metadata.size() > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        append_u32(out, static_cast<std::uint32_t>(reply.metadata.size()));
        for (const auto &[key, value] : reply.metadata) {
            if (!append_string(out, key) || !append_string(out, value)) {
                return false;
            }
        }
        return append_bytes(out, reply.payload);
    }

    bool encode_tunnel_registration(const TunnelRegistration &registration, yuan::rpc::Bytes &out)
    {
        constexpr std::uint32_t registration_version = 1;
        out.clear();
        append_u32(out, registration_version);
        append_u64(out, registration.service_id);
        append_u32(out, registration.port);
        return append_string(out, registration.name);
    }

    std::optional<TunnelEnvelope> decode_tunnel_envelope(const yuan::rpc::Bytes &in)
    {
        TunnelEnvelope envelope;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        std::uint32_t target_type = 0;
        std::uint32_t mode = 0;
        std::uint32_t metadata_size = 0;
        if (!read_u32(in, offset, version) || version != 1 ||
            !read_string(in, offset, envelope.source) ||
            !read_string(in, offset, envelope.target) ||
            !read_u64(in, offset, envelope.source_service_id) ||
            !read_u64(in, offset, envelope.target_service_id) ||
            !read_u32(in, offset, target_type) ||
            !read_u32(in, offset, mode) ||
            !read_u64(in, offset, envelope.request_id) ||
            !read_u64(in, offset, envelope.continuation_id) ||
            !read_u32(in, offset, envelope.route.service) ||
            !read_u32(in, offset, envelope.route.method) ||
            !read_string(in, offset, envelope.route.name) ||
            !read_u32(in, offset, metadata_size)) {
            return std::nullopt;
        }
        envelope.target_type = static_cast<GameServiceType>(target_type);
        envelope.mode = static_cast<TunnelEnvelope::ForwardMode>(mode);
        for (std::uint32_t i = 0; i < metadata_size; ++i) {
            std::string key;
            std::string value;
            if (!read_string(in, offset, key) || !read_string(in, offset, value)) {
                return std::nullopt;
            }
            envelope.metadata.emplace(std::move(key), std::move(value));
        }
        if (!read_bytes(in, offset, envelope.payload) || offset != in.size()) {
            return std::nullopt;
        }
        return envelope;
    }

    std::optional<TunnelReply> decode_tunnel_reply(const yuan::rpc::Bytes &in)
    {
        TunnelReply reply;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        std::uint32_t status = 0;
        std::uint32_t metadata_size = 0;
        if (!read_u32(in, offset, version) || version != 1 ||
            !read_string(in, offset, reply.source) ||
            !read_string(in, offset, reply.target) ||
            !read_u64(in, offset, reply.source_service_id) ||
            !read_u64(in, offset, reply.target_service_id) ||
            !read_u64(in, offset, reply.request_id) ||
            !read_u64(in, offset, reply.continuation_id) ||
            !read_u32(in, offset, status) ||
            !read_string(in, offset, reply.error) ||
            !read_u32(in, offset, metadata_size)) {
            return std::nullopt;
        }
        reply.status = static_cast<yuan::rpc::RpcStatus>(status);
        for (std::uint32_t i = 0; i < metadata_size; ++i) {
            std::string key;
            std::string value;
            if (!read_string(in, offset, key) || !read_string(in, offset, value)) {
                return std::nullopt;
            }
            reply.metadata.emplace(std::move(key), std::move(value));
        }
        if (!read_bytes(in, offset, reply.payload) || offset != in.size()) {
            return std::nullopt;
        }
        return reply;
    }

    std::optional<TunnelRegistration> decode_tunnel_registration(const yuan::rpc::Bytes &in)
    {
        TunnelRegistration registration;
        std::size_t offset = 0;
        std::uint32_t version = 0;
        std::uint32_t port = 0;
        if (!read_u32(in, offset, version) || version != 1 ||
            !read_u64(in, offset, registration.service_id) ||
            !read_u32(in, offset, port) ||
            !read_string(in, offset, registration.name) || offset != in.size()) {
            return std::nullopt;
        }
        registration.port = static_cast<std::uint16_t>(port);
        return registration;
    }

    TunnelService::TunnelService(ServiceAddress address)
        : ServiceNode(std::move(address))
    {
        yuan::rpc::Route forward_route;
        forward_route.name = std::string(route::tunnel_forward);
        (void)rpc_server().register_handler(forward_route, [this](const yuan::rpc::Message &message) {
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
            auto response = forward(std::move(forwarded));
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            return response;
        });

        yuan::rpc::Route reply_route;
        reply_route.name = std::string(route::tunnel_reply);
        (void)rpc_server().register_handler(reply_route, [this](const yuan::rpc::Message &message) {
            const auto reply = decode_tunnel_reply(message.payload);
            if (!reply) {
                yuan::rpc::Response response;
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid tunnel reply";
                return response;
            }
            return handle_reply(*reply);
        });
    }

    bool TunnelService::register_endpoint(ServiceAddress address, yuan::rpc::Server &server)
    {
        const auto key = service_key(address);
        if (key.empty()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto endpoint_address = address;
        endpoints_[key] = Endpoint{std::move(address), [&server](yuan::rpc::Message message) {
                                       return server.handle(message);
                                   }};
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
        response.metadata["tunnel.instance"] = service_key(address());
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
        last_response.metadata["tunnel.broadcast.count"] = std::to_string(endpoints.size());
        last_response.metadata["tunnel.broadcast.ok"] = std::to_string(ok_count);
        last_response.metadata["tunnel.instance"] = service_key(address());
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
        message.metadata["tunnel.source"] = envelope.source;
        message.metadata["tunnel.target"] = envelope.target;
        message.metadata["tunnel.source_service_id"] = std::to_string(envelope.source_service_id);
        message.metadata["tunnel.target_service_id"] = std::to_string(envelope.target_service_id);
        message.metadata["tunnel.origin.request_id"] = std::to_string(envelope.request_id);
        message.metadata["tunnel.origin.continuation_id"] = std::to_string(envelope.continuation_id);
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
        forwarded.metadata["tunnel.reply.source"] = std::move(reply.source);
        forwarded.metadata["tunnel.reply.target"] = std::move(reply.target);
        forwarded.metadata["tunnel.reply.source_service_id"] = std::to_string(reply.source_service_id);
        forwarded.metadata["tunnel.reply.target_service_id"] = std::to_string(reply.target_service_id);
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
