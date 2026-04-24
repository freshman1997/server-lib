#ifndef __NET_HTTP2_FRAME_CODEC_H__
#define __NET_HTTP2_FRAME_CODEC_H__

#include "buffer/byte_buffer.h"
#include "http2/types.h"

#include <optional>

namespace yuan::net::http::http2
{
    class FrameCodec
    {
    public:
        static std::optional<Frame> try_decode(::yuan::buffer::ByteBuffer &buffer,
                                                std::uint32_t max_frame_size = kDefaultMaxFrameSize);

        static void encode_frame(const Frame &frame, ::yuan::buffer::ByteBuffer &out);

        static Frame make_settings_ack();
        static Frame make_ping_ack(const Frame &ping_frame);
        static Frame make_goaway(ErrorCode error, std::uint32_t last_stream_id = 0);
    };
}

#endif
