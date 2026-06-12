#include "yuan/rpc/wire.h"

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

        bool write_string(Bytes &out, std::string_view value)
        {
            if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
                return false;
            }
            write_u16(out, static_cast<std::uint16_t>(value.size()));
            out.insert(out.end(), value.begin(), value.end());
            return true;
        }

        bool read_string(const std::uint8_t *data, std::size_t size, std::size_t &offset, std::string &value)
        {
            std::uint16_t len = 0;
            if (!read_u16(data, size, offset, len) || offset + len > size) {
                return false;
            }
            value.assign(reinterpret_cast<const char *>(data + offset), len);
            offset += len;
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
    }

    bool encode_metadata(const Metadata &metadata, Bytes &out)
    {
        if (metadata.size() > std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
        write_u16(out, static_cast<std::uint16_t>(metadata.size()));
        for (const auto &[key, value] : metadata) {
            if (!write_string(out, key) || !write_string(out, value)) {
                return false;
            }
        }
        return true;
    }

    bool decode_metadata(const std::uint8_t *data, std::size_t size, Metadata &metadata)
    {
        std::size_t offset = 0;
        std::uint16_t count = 0;
        if (!read_u16(data, size, offset, count)) {
            return false;
        }
        Metadata out;
        out.reserve(count);
        for (std::uint16_t i = 0; i < count; ++i) {
            std::string key;
            std::string value;
            if (!read_string(data, size, offset, key) || !read_string(data, size, offset, value)) {
                return false;
            }
            out.emplace(std::move(key), std::move(value));
        }
        if (offset != size) {
            return false;
        }
        metadata = std::move(out);
        return true;
    }

    bool encode_frame(const FrameHeader &header,
                      const Metadata &metadata,
                      std::string_view route_name,
                      std::string_view error,
                      const Bytes &payload,
                      Bytes &out,
                      const EncodeOptions &options)
    {
        Bytes metadata_bytes;
        if (!encode_metadata(metadata, metadata_bytes)) {
            return false;
        }
        if (route_name.size() > std::numeric_limits<std::uint32_t>::max() ||
            error.size() > std::numeric_limits<std::uint32_t>::max() ||
            metadata_bytes.size() > std::numeric_limits<std::uint32_t>::max() ||
            payload.size() > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }

        Bytes plain_body;
        plain_body.reserve(route_name.size() + metadata_bytes.size() + error.size() + payload.size());
        append_checked(plain_body, route_name);
        append_checked(plain_body, metadata_bytes);
        append_checked(plain_body, error);
        append_checked(plain_body, payload);

        Bytes body;
        if (header.encryption == Encryption::none) {
            body = std::move(plain_body);
        } else {
            if (!options.encrypt) {
                return false;
            }
            CryptoContext context;
            context.encryption = header.encryption;
            context.key_id = header.key_id;
            context.nonce = header.nonce;
            context.kind = header.kind;
            context.request_id = header.request_id;
            if (!options.encrypt(context, plain_body, body)) {
                return false;
            }
        }

        if (body.size() > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }

        const std::uint32_t body_size = static_cast<std::uint32_t>(body.size());
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
        write_u32(out, static_cast<std::uint32_t>(route_name.size()));
        write_u32(out, static_cast<std::uint32_t>(metadata_bytes.size()));
        write_u32(out, static_cast<std::uint32_t>(error.size()));
        write_u32(out, static_cast<std::uint32_t>(payload.size()));
        write_u64(out, header.nonce);
        write_u64(out, header.coroutine_id);
        write_u32(out, header.key_id);
        append_checked(out, body);
        return out.size() == header_size + body_size;
    }

    bool encode_message(const Message &message, Bytes &out, const EncodeOptions &options)
    {
        FrameHeader header;
        header.kind = message.kind;
        header.request_id = message.request_id;
        header.coroutine_id = message.coroutine_id;
        header.serialization = message.serialization;
        header.compression = message.compression;
        header.encryption = message.encryption;
        header.key_id = message.key_id;
        header.nonce = message.nonce;
        header.service = message.route.service;
        header.method = message.route.method;
        return encode_frame(header, message.metadata, message.route.name, {}, message.payload, out, options);
    }

    bool encode_response(const Response &response, Bytes &out, const EncodeOptions &options)
    {
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
        return encode_frame(header, response.metadata, {}, response.error, response.payload, out, options);
    }

    DecodeResult decode_frame(const std::uint8_t *data, std::size_t size, const DecodeOptions &options)
    {
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
            !read_u32(data, size, offset, header.route_name_size) ||
            !read_u32(data, size, offset, header.metadata_size) ||
            !read_u32(data, size, offset, header.error_size) ||
            !read_u32(data, size, offset, header.payload_size)) {
            result.error = DecodeError::malformed;
            return result;
        }

        if (!read_u64(data, size, offset, header.nonce) || !read_u64(data, size, offset, header.coroutine_id) ||
            !read_u32(data, size, offset, header.key_id)) {
            result.error = DecodeError::malformed;
            return result;
        }

        const std::uint64_t section_sum = static_cast<std::uint64_t>(header.route_name_size) +
                                          header.metadata_size + header.error_size + header.payload_size;

        Bytes plain_body;
        const auto *body = data + header_size;
        if (header.encryption == Encryption::none) {
            if (section_sum != body_size) {
                result.error = DecodeError::malformed;
                return result;
            }
            plain_body.assign(body, body + body_size);
        } else {
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
        }
        if (plain_body.size() != section_sum) {
            result.error = DecodeError::malformed;
            return result;
        }

        body = plain_body.data();
        std::size_t body_offset = 0;
        result.frame.route_name.assign(reinterpret_cast<const char *>(body + body_offset), header.route_name_size);
        body_offset += header.route_name_size;
        if (!decode_metadata(body + body_offset, header.metadata_size, result.frame.metadata)) {
            result.error = DecodeError::malformed;
            return result;
        }
        body_offset += header.metadata_size;
        result.frame.error.assign(reinterpret_cast<const char *>(body + body_offset), header.error_size);
        body_offset += header.error_size;
        result.frame.payload.assign(body + body_offset, body + body_offset + header.payload_size);

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
        Message message;
        message.kind = frame.header.kind;
        message.request_id = frame.header.request_id;
        message.coroutine_id = frame.header.coroutine_id;
        message.route.service = frame.header.service;
        message.route.method = frame.header.method;
        message.route.name = std::move(frame.route_name);
        message.serialization = frame.header.serialization;
        message.compression = frame.header.compression;
        message.encryption = frame.header.encryption;
        message.key_id = frame.header.key_id;
        message.nonce = frame.header.nonce;
        message.metadata = std::move(frame.metadata);
        message.payload = std::move(frame.payload);
        return message;
    }

    Response to_response(DecodedFrame frame)
    {
        Response response;
        response.request_id = frame.header.request_id;
        response.coroutine_id = frame.header.coroutine_id;
        response.status = frame.header.status;
        response.error = std::move(frame.error);
        response.serialization = frame.header.serialization;
        response.compression = frame.header.compression;
        response.encryption = frame.header.encryption;
        response.key_id = frame.header.key_id;
        response.nonce = frame.header.nonce;
        response.metadata = std::move(frame.metadata);
        response.payload = std::move(frame.payload);
        return response;
    }
}
