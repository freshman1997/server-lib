#include "http2/frame_codec.h"

#include <cstring>

namespace yuan::net::http::http2
{
    namespace
    {
        std::uint32_t read_u24(const char *p)
        {
            return (static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(p[2]));
        }

        void write_u24(::yuan::buffer::ByteBuffer &out, std::uint32_t v)
        {
            const std::uint8_t b0 = static_cast<std::uint8_t>((v >> 16) & 0xff);
            const std::uint8_t b1 = static_cast<std::uint8_t>((v >> 8) & 0xff);
            const std::uint8_t b2 = static_cast<std::uint8_t>(v & 0xff);
            out.append_u8(b0);
            out.append_u8(b1);
            out.append_u8(b2);
        }
    }

    std::optional<Frame> FrameCodec::try_decode(::yuan::buffer::ByteBuffer &buffer,
                                                  std::uint32_t max_frame_size)
    {
        if (buffer.readable_bytes() < 9) {
            return std::nullopt;
        }

        const auto span = buffer.readable_span();
        const std::uint32_t length = read_u24(span.data());
        if (length > max_frame_size) {
            return std::nullopt;
        }

        if (buffer.readable_bytes() < static_cast<std::size_t>(9 + length)) {
            return std::nullopt;
        }

        Frame frame;
        frame.header.length = length;
        frame.header.type = static_cast<FrameType>(static_cast<std::uint8_t>(span[3]));
        frame.header.flags = static_cast<std::uint8_t>(span[4]);
        frame.header.stream_id =
            ((static_cast<std::uint32_t>(static_cast<unsigned char>(span[5])) << 24) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(span[6])) << 16) |
             (static_cast<std::uint32_t>(static_cast<unsigned char>(span[7])) << 8) |
             static_cast<std::uint32_t>(static_cast<unsigned char>(span[8]))) &
            0x7fffffffU;

        buffer.consume(9);
        frame.payload.resize(length);
        if (length > 0) {
            const auto payload_span = buffer.readable_span();
            std::memcpy(frame.payload.data(), payload_span.data(), length);
            buffer.consume(length);
        }

        return frame;
    }

    void FrameCodec::encode_frame(const Frame &frame, ::yuan::buffer::ByteBuffer &out)
    {
        write_u24(out, frame.header.length);
        out.append_u8(static_cast<std::uint8_t>(frame.header.type));
        out.append_u8(frame.header.flags);
        out.append_u32(frame.header.stream_id & 0x7fffffffU);
        if (!frame.payload.empty()) {
            out.append(std::span<const char>(reinterpret_cast<const char *>(frame.payload.data()), frame.payload.size()));
        }
    }

    Frame FrameCodec::make_settings_ack()
    {
        Frame frame;
        frame.header.length = 0;
        frame.header.type = FrameType::settings;
        frame.header.flags = flag_ack;
        frame.header.stream_id = 0;
        return frame;
    }

    Frame FrameCodec::make_ping_ack(const Frame &ping_frame)
    {
        Frame frame;
        frame.header.length = 8;
        frame.header.type = FrameType::ping;
        frame.header.flags = flag_ack;
        frame.header.stream_id = 0;
        frame.payload = ping_frame.payload;
        frame.payload.resize(8);
        return frame;
    }

    Frame FrameCodec::make_goaway(ErrorCode error, std::uint32_t last_stream_id)
    {
        Frame frame;
        frame.header.length = 8;
        frame.header.type = FrameType::goaway;
        frame.header.flags = flag_none;
        frame.header.stream_id = 0;
        frame.payload.resize(8);

        frame.payload[0] = static_cast<std::uint8_t>((last_stream_id >> 24) & 0x7f);
        frame.payload[1] = static_cast<std::uint8_t>((last_stream_id >> 16) & 0xff);
        frame.payload[2] = static_cast<std::uint8_t>((last_stream_id >> 8) & 0xff);
        frame.payload[3] = static_cast<std::uint8_t>(last_stream_id & 0xff);

        const auto err = static_cast<std::uint32_t>(error);
        frame.payload[4] = static_cast<std::uint8_t>((err >> 24) & 0xff);
        frame.payload[5] = static_cast<std::uint8_t>((err >> 16) & 0xff);
        frame.payload[6] = static_cast<std::uint8_t>((err >> 8) & 0xff);
        frame.payload[7] = static_cast<std::uint8_t>(err & 0xff);
        return frame;
    }
}
