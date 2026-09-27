#include "yuan/rpc/wire.h"

#include "yuan/rpc/profiling.h"

#include <limits>
#include <utility>

namespace yuan::rpc::wire
{
    namespace
    {
        void write_u8(Bytes &out, std::uint8_t value)
        {
            out.push_back(value);
        }

        void write_u16(Bytes &out, std::uint16_t value)
        {
            out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
            out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        }

        void write_u32(Bytes &out, std::uint32_t value)
        {
            out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
            out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
            out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
            out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        }

        void write_u64(Bytes &out, std::uint64_t value)
        {
            for (int shift = 56; shift >= 0; shift -= 8) {
                out.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFU));
            }
        }

        bool read_u8(const std::uint8_t *data, std::size_t size, std::size_t &offset, std::uint8_t &value)
        {
            if (offset + 1 > size) {
                return false;
            }
            value = data[offset++];
            return true;
        }

        bool read_u16(const std::uint8_t *data, std::size_t size, std::size_t &offset, std::uint16_t &value)
        {
            if (offset + 2 > size) {
                return false;
            }
            value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8U) |
                                               static_cast<std::uint16_t>(data[offset + 1]));
            offset += 2;
            return true;
        }

        bool read_u32(const std::uint8_t *data, std::size_t size, std::size_t &offset, std::uint32_t &value)
        {
            if (offset + 4 > size) {
                return false;
            }
            value = (static_cast<std::uint32_t>(data[offset]) << 24U) |
                    (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
                    (static_cast<std::uint32_t>(data[offset + 2]) << 8U) |
                    static_cast<std::uint32_t>(data[offset + 3]);
            offset += 4;
            return true;
        }

        bool read_u64(const std::uint8_t *data, std::size_t size, std::size_t &offset, std::uint64_t &value)
        {
            if (offset + 8 > size) {
                return false;
            }
            value = 0;
            for (int i = 0; i < 8; ++i) {
                value = (value << 8U) | data[offset + static_cast<std::size_t>(i)];
            }
            offset += 8;
            return true;
        }

        void append_checked(Bytes &out, const Bytes &value)
        {
            out.insert(out.end(), value.begin(), value.end());
        }

        void append_checked(Bytes &out, std::string_view value)
        {
            out.insert(out.end(), value.begin(), value.end());
        }

        void write_header(::yuan::buffer::ByteBuffer &out, const FrameHeader &header, std::uint32_t body_size)
        {
            out.append_u32(magic);
            out.append_u8(version);
            out.append_u8(static_cast<std::uint8_t>(header.kind));
            out.append_u8(static_cast<std::uint8_t>(header.serialization));
            out.append_u8(static_cast<std::uint8_t>(header.compression));
            out.append_u16(static_cast<std::uint16_t>(header.status));
            out.append_u16(static_cast<std::uint16_t>(header.encryption));
            out.append_u32(body_size);
            out.append_u64(header.request_id);
            out.append_u32(header.service);
            out.append_u32(header.method);
            out.append_u32(body_size);
            out.append_u64(header.session_id);
            out.append_u64(header.peer_service_id);
            out.append_u64(header.auth_token);
            out.append_u32(header.flags);
            out.append_u64(header.nonce);
            out.append_u64(header.coroutine_id);
            out.append_u32(header.key_id);
        }

        bool encode_frame_to_buffer(const FrameHeader &header,
                                    const Bytes &payload,
                                    ::yuan::buffer::ByteBuffer &out,
                                    const EncodeOptions &options)
        {
            if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }

            Bytes encrypted_body;
            const Bytes *body = &payload;
            if (header.encryption != Encryption::none) {
                if (!options.encrypt) {
                    return false;
                }
                CryptoContext context;
                context.encryption = header.encryption;
                context.key_id = header.key_id;
                context.nonce = header.nonce;
                context.kind = header.kind;
                context.request_id = header.request_id;
                if (!options.encrypt(context, payload, encrypted_body)) {
                    return false;
                }
                body = &encrypted_body;
            }
            if (body->size() > std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }

            const auto body_size = static_cast<std::uint32_t>(body->size());
            out.clear();
            out.ensure_writable(header_size + body_size);
            write_header(out, header, body_size);
            if (!body->empty()) {
                out.append(body->data(), body->size());
            }
            return out.readable_bytes() == header_size + body_size;
        }
    }

    bool encode_frame(const FrameHeader &header,
                      const Metadata &metadata,
                      std::string_view route_name,
                      std::string_view error,
                      const Bytes &payload,
                      Bytes &out,
                      const EncodeOptions &options)
    {
        YUAN_RPC_PROFILE_ZONE("yuan.rpc.wire.encode_frame");
        (void)metadata;
        (void)route_name;
        (void)error;
        if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }

        Bytes encrypted_body;
        const Bytes *body = &payload;
        if (header.encryption == Encryption::none) {
        } else {
            YUAN_RPC_PROFILE_ZONE("yuan.rpc.wire.encrypt");
            if (!options.encrypt) {
                return false;
            }
            CryptoContext context;
            context.encryption = header.encryption;
            context.key_id = header.key_id;
            context.nonce = header.nonce;
            context.kind = header.kind;
            context.request_id = header.request_id;
            if (!options.encrypt(context, payload, encrypted_body)) {
                return false;
            }
            body = &encrypted_body;
        }

        if (body->size() > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }

        const std::uint32_t body_size = static_cast<std::uint32_t>(body->size());
        out.clear();
        out.reserve(header_size + body_size);
        write_u32(out, magic);
        write_u8(out, version);
        write_u8(out, static_cast<std::uint8_t>(header.kind));
        write_u8(out, static_cast<std::uint8_t>(header.serialization));
        write_u8(out, static_cast<std::uint8_t>(header.compression));
        write_u16(out, static_cast<std::uint16_t>(header.status));
        write_u16(out, static_cast<std::uint16_t>(header.encryption));
        write_u32(out, body_size);
        write_u64(out, header.request_id);
        write_u32(out, header.service);
        write_u32(out, header.method);
        write_u32(out, body_size);
        write_u64(out, header.session_id);
        write_u64(out, header.peer_service_id);
        write_u64(out, header.auth_token);
        write_u32(out, header.flags);
        write_u64(out, header.nonce);
        write_u64(out, header.coroutine_id);
        write_u32(out, header.key_id);
        append_checked(out, *body);
        return out.size() == header_size + body_size;
    }

    bool encode_message(const Message &message, Bytes &out, const EncodeOptions &options)
    {
        YUAN_RPC_PROFILE_ZONE("yuan.rpc.wire.encode_message");
        FrameHeader header;
        header.kind = message.kind;
        header.request_id = message.request_id;
        header.coroutine_id = message.coroutine_id;
        header.serialization = message.serialization;
        header.compression = message.compression;
        header.encryption = message.encryption;
        header.key_id = message.key_id;
        header.nonce = message.nonce;
        header.session_id = message.session_id;
        header.peer_service_id = message.peer_service_id;
        header.auth_token = message.auth_token;
        header.flags = message.flags;
        header.service = message.route.service;
        header.method = message.route.method;
        return encode_frame(header, message.metadata, message.route.name, {}, message.payload, out, options);
    }

    bool encode_response(const Response &response, Bytes &out, const EncodeOptions &options)
    {
        YUAN_RPC_PROFILE_ZONE("yuan.rpc.wire.encode_response");
        FrameHeader header;
        header.kind = MessageKind::response;
        header.request_id = response.request_id;
        header.coroutine_id = response.coroutine_id;
        header.status = response.status;
        header.serialization = response.serialization;
        header.compression = response.compression;
        header.encryption = response.encryption;
        header.key_id = response.key_id;
        header.nonce = response.nonce;
        header.session_id = response.session_id;
        header.peer_service_id = response.peer_service_id;
        header.auth_token = response.auth_token;
        header.flags = response.flags;
        return encode_frame(header, response.metadata, {}, response.error, response.payload, out, options);
    }

    bool encode_message(const Message &message, ::yuan::buffer::ByteBuffer &out, const EncodeOptions &options)
    {
        YUAN_RPC_PROFILE_ZONE("yuan.rpc.wire.encode_message");
        FrameHeader header;
        header.kind = message.kind;
        header.request_id = message.request_id;
        header.coroutine_id = message.coroutine_id;
        header.serialization = message.serialization;
        header.compression = message.compression;
        header.encryption = message.encryption;
        header.key_id = message.key_id;
        header.nonce = message.nonce;
        header.session_id = message.session_id;
        header.peer_service_id = message.peer_service_id;
        header.auth_token = message.auth_token;
        header.flags = message.flags;
        header.service = message.route.service;
        header.method = message.route.method;
        return encode_frame_to_buffer(header, message.payload, out, options);
    }

    bool encode_response(const Response &response, ::yuan::buffer::ByteBuffer &out, const EncodeOptions &options)
    {
        YUAN_RPC_PROFILE_ZONE("yuan.rpc.wire.encode_response");
        FrameHeader header;
        header.kind = MessageKind::response;
        header.request_id = response.request_id;
        header.coroutine_id = response.coroutine_id;
        header.status = response.status;
        header.serialization = response.serialization;
        header.compression = response.compression;
        header.encryption = response.encryption;
        header.key_id = response.key_id;
        header.nonce = response.nonce;
        header.session_id = response.session_id;
        header.peer_service_id = response.peer_service_id;
        header.auth_token = response.auth_token;
        header.flags = response.flags;
        return encode_frame_to_buffer(header, response.payload, out, options);
    }

    DecodeResult decode_frame(const std::uint8_t *data, std::size_t size, const DecodeOptions &options)
    {
        YUAN_RPC_PROFILE_ZONE("yuan.rpc.wire.decode_frame");
        DecodeResult result;
        if (size < header_size) {
            result.error = DecodeError::need_more;
            return result;
        }

        std::size_t offset = 0;
        std::uint32_t got_magic = 0;
        std::uint8_t got_version = 0;
        std::uint8_t kind = 0;
        std::uint8_t serialization = 0;
        std::uint8_t compression = 0;
        std::uint16_t status = 0;
        std::uint16_t encryption = 0;
        std::uint32_t body_size = 0;
        if (!read_u32(data, size, offset, got_magic) || got_magic != magic) {
            result.error = DecodeError::bad_magic;
            return result;
        }
        if (!read_u8(data, size, offset, got_version) || got_version != version) {
            result.error = DecodeError::unsupported_version;
            return result;
        }
        if (!read_u8(data, size, offset, kind) || !read_u8(data, size, offset, serialization) ||
            !read_u8(data, size, offset, compression) || !read_u16(data, size, offset, status) ||
            !read_u16(data, size, offset, encryption) || !read_u32(data, size, offset, body_size)) {
            result.error = DecodeError::malformed;
            return result;
        }
        if (body_size > options.max_frame_size || body_size > std::numeric_limits<std::size_t>::max() - header_size) {
            result.error = DecodeError::frame_too_large;
            return result;
        }
        if (size < header_size + body_size) {
            result.error = DecodeError::need_more;
            return result;
        }

        auto &header = result.frame.header;
        header.kind = static_cast<MessageKind>(kind);
        header.serialization = static_cast<Serialization>(serialization);
        header.compression = static_cast<Compression>(compression);
        header.encryption = static_cast<Encryption>(encryption);
        header.status = static_cast<RpcStatus>(status);
        if (!read_u64(data, size, offset, header.request_id) ||
            !read_u32(data, size, offset, header.service) ||
            !read_u32(data, size, offset, header.method) ||
            !read_u32(data, size, offset, header.payload_size)) {
            result.error = DecodeError::malformed;
            return result;
        }

        if (!read_u64(data, size, offset, header.session_id) || !read_u64(data, size, offset, header.peer_service_id) ||
            !read_u64(data, size, offset, header.auth_token) ||
            !read_u32(data, size, offset, header.flags) ||
            !read_u64(data, size, offset, header.nonce) || !read_u64(data, size, offset, header.coroutine_id) ||
            !read_u32(data, size, offset, header.key_id)) {
            result.error = DecodeError::malformed;
            return result;
        }

        Bytes plain_body;
        const auto *body = data + header_size;
        if (header.encryption == Encryption::none) {
            if (header.payload_size != body_size) {
                result.error = DecodeError::malformed;
                return result;
            }
        } else {
            YUAN_RPC_PROFILE_ZONE("yuan.rpc.wire.decrypt");
            if (!options.decrypt) {
                result.error = DecodeError::malformed;
                return result;
            }
            Bytes encrypted_body(body, body + body_size);
            CryptoContext context;
            context.encryption = header.encryption;
            context.key_id = header.key_id;
            context.nonce = header.nonce;
            context.kind = header.kind;
            context.request_id = header.request_id;
            if (!options.decrypt(context, encrypted_body, plain_body)) {
                result.error = DecodeError::malformed;
                return result;
            }
            if (plain_body.size() != header.payload_size) {
                result.error = DecodeError::malformed;
                return result;
            }
            body = plain_body.data();
        }

        {
            YUAN_RPC_PROFILE_ZONE("yuan.rpc.wire.payload_assign");
            result.frame.payload.assign(body, body + header.payload_size);
        }

        result.ok = true;
        result.consumed = header_size + body_size;
        return result;
    }

    DecodeResult decode_frame(const Bytes &bytes, const DecodeOptions &options)
    {
        return decode_frame(bytes.data(), bytes.size(), options);
    }

    Message to_message(DecodedFrame frame)
    {
        YUAN_RPC_PROFILE_ZONE("yuan.rpc.wire.to_message");
        Message message;
        message.kind = frame.header.kind;
        message.request_id = frame.header.request_id;
        message.coroutine_id = frame.header.coroutine_id;
        message.connection_id = 0;
        message.session_id = frame.header.session_id;
        message.peer_service_id = frame.header.peer_service_id;
        message.auth_token = frame.header.auth_token;
        message.flags = frame.header.flags;
        message.route.service = frame.header.service;
        message.route.method = frame.header.method;
        message.serialization = frame.header.serialization;
        message.compression = frame.header.compression;
        message.encryption = frame.header.encryption;
        message.key_id = frame.header.key_id;
        message.nonce = frame.header.nonce;
        message.payload = std::move(frame.payload);
        return message;
    }

    Response to_response(DecodedFrame frame)
    {
        YUAN_RPC_PROFILE_ZONE("yuan.rpc.wire.to_response");
        Response response;
        response.request_id = frame.header.request_id;
        response.coroutine_id = frame.header.coroutine_id;
        response.session_id = frame.header.session_id;
        response.peer_service_id = frame.header.peer_service_id;
        response.auth_token = frame.header.auth_token;
        response.flags = frame.header.flags;
        response.status = frame.header.status;
        response.serialization = frame.header.serialization;
        response.compression = frame.header.compression;
        response.encryption = frame.header.encryption;
        response.key_id = frame.header.key_id;
        response.nonce = frame.header.nonce;
        response.payload = std::move(frame.payload);
        return response;
    }
}
