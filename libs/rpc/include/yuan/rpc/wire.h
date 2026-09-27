#ifndef YUAN_RPC_WIRE_H
#define YUAN_RPC_WIRE_H

#include "types.h"
#include "buffer/byte_buffer.h"

#include <optional>
#include <functional>

namespace yuan::rpc::wire
{
    inline constexpr std::uint32_t magic = 0x59525043U; // YRPC
    inline constexpr std::uint8_t version = 1;
    inline constexpr std::uint32_t max_default_frame_size = 64U * 1024U * 1024U;

    enum class DecodeError
    {
        need_more,
        bad_magic,
        unsupported_version,
        frame_too_large,
        malformed
    };

    struct FrameHeader
    {
        MessageKind kind = MessageKind::request;
        RequestId request_id = 0;
        CoroutineId coroutine_id = 0;
        RpcStatus status = RpcStatus::ok;
        Serialization serialization = Serialization::raw;
        Compression compression = Compression::none;
        Encryption encryption = Encryption::none;
        std::uint32_t key_id = 0;
        std::uint64_t nonce = 0;
        std::uint64_t session_id = 0;
        std::uint64_t peer_service_id = 0;
        std::uint64_t auth_token = 0;
        std::uint32_t flags = 0;
        ServiceId service = 0;
        MethodId method = 0;
        std::uint32_t payload_size = 0;
    };

    struct DecodedFrame
    {
        FrameHeader header;
        Bytes payload;
    };

    struct CryptoContext
    {
        Encryption encryption = Encryption::none;
        std::uint32_t key_id = 0;
        std::uint64_t nonce = 0;
        MessageKind kind = MessageKind::request;
        RequestId request_id = 0;
    };

    using Cipher = std::function<bool(const CryptoContext &, const Bytes &, Bytes &)>;

    struct EncodeOptions
    {
        Cipher encrypt;
    };

    struct DecodeOptions
    {
        std::uint32_t max_frame_size = max_default_frame_size;
        Cipher decrypt;
    };

    struct DecodeResult
    {
        bool ok = false;
        DecodeError error = DecodeError::need_more;
        std::size_t consumed = 0;
        DecodedFrame frame;
    };

    inline constexpr std::size_t header_size = 84;

    bool encode_frame(const FrameHeader &header, const Metadata &metadata, std::string_view route_name, std::string_view error, const Bytes &payload, Bytes &out, const EncodeOptions &options = {});
    
    bool encode_message(const Message &message, Bytes &out, const EncodeOptions &options = {});

    bool encode_response(const Response &response, Bytes &out, const EncodeOptions &options = {});

    bool encode_message(const Message &message, ::yuan::buffer::ByteBuffer &out, const EncodeOptions &options = {});

    bool encode_response(const Response &response, ::yuan::buffer::ByteBuffer &out, const EncodeOptions &options = {});

    DecodeResult decode_frame(const std::uint8_t *data, std::size_t size, const DecodeOptions &options = {});

    DecodeResult decode_frame(const Bytes &bytes, const DecodeOptions &options = {});

    Message to_message(DecodedFrame frame);

    Response to_response(DecodedFrame frame);
}

#endif
