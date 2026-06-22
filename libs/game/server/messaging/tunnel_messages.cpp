#include "messaging/tunnel_messages.h"

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
        constexpr std::uint32_t registration_version = 2;
        out.clear();
        append_u32(out, registration_version);
        append_u64(out, registration.service_id);
        if (!append_string(out, registration.host)) {
            return false;
        }

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
        if (!read_u32(in, offset, version) || (version != 1 && version != 2) ||
            !read_u64(in, offset, registration.service_id)) {
            return std::nullopt;
        }

        if (version == 2 && !read_string(in, offset, registration.host)) {
            return std::nullopt;
        }

        if (!read_u32(in, offset, port) || !read_string(in, offset, registration.name) || offset != in.size()) {
            return std::nullopt;
        }

        registration.port = static_cast<std::uint16_t>(port);
        return registration;
    }

}
