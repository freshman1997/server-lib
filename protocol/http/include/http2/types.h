#ifndef __NET_HTTP2_TYPES_H__
#define __NET_HTTP2_TYPES_H__

#include <cstdint>
#include <vector>

namespace yuan::net::http::http2
{
    static constexpr std::uint32_t kDefaultMaxFrameSize = 16384;

    enum class FrameType : std::uint8_t
    {
        data = 0x0,
        headers = 0x1,
        priority = 0x2,
        rst_stream = 0x3,
        settings = 0x4,
        push_promise = 0x5,
        ping = 0x6,
        goaway = 0x7,
        window_update = 0x8,
        continuation = 0x9
    };

    enum class ErrorCode : std::uint32_t
    {
        no_error = 0x0,
        protocol_error = 0x1,
        internal_error = 0x2,
        flow_control_error = 0x3,
        settings_timeout = 0x4,
        stream_closed = 0x5,
        frame_size_error = 0x6,
        refused_stream = 0x7,
        cancel = 0x8,
        compression_error = 0x9,
        connect_error = 0xa,
        enhance_your_calm = 0xb,
        inadequate_security = 0xc,
        http_1_1_required = 0xd
    };

    enum FrameFlags : std::uint8_t
    {
        flag_none = 0x00,
        flag_end_stream = 0x01,
        flag_ack = 0x01,
        flag_end_headers = 0x04,
        flag_padded = 0x08,
        flag_priority = 0x20
    };

    struct FrameHeader
    {
        std::uint32_t length = 0;
        FrameType type = FrameType::data;
        std::uint8_t flags = 0;
        std::uint32_t stream_id = 0;
    };

    struct Frame
    {
        FrameHeader header;
        std::vector<std::uint8_t> payload;
    };
}

#endif
