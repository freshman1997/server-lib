#include "rtcp_packet.h"

#include "endian/endian.hpp"

#include <cstddef>
#include <stdint.h>

namespace yuan::net::rtcp
{

namespace
{

constexpr std::size_t kRtcpHeaderSize = 4;

void append_u24(::yuan::buffer::ByteBuffer &buffer, uint32_t value)
{
    buffer.append_u8(static_cast<uint8_t>((value >> 16) & 0xFF));
    buffer.append_u8(static_cast<uint8_t>((value >> 8) & 0xFF));
    buffer.append_u8(static_cast<uint8_t>(value & 0xFF));
}

uint32_t decode_u24(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 16) |
           (static_cast<uint32_t>(data[1]) << 8) |
           static_cast<uint32_t>(data[2]);
}

bool read_u8(const uint8_t *data, std::size_t size, std::size_t &offset, uint8_t &out)
{
    if (offset + 1 > size) {
        return false;
    }
    out = data[offset++];
    return true;
}

bool read_u16(const uint8_t *data, std::size_t size, std::size_t &offset, uint16_t &out)
{
    if (offset + 2 > size) {
        return false;
    }
    out = static_cast<uint16_t>(data[offset] << 8) | static_cast<uint16_t>(data[offset + 1]);
    offset += 2;
    return true;
}

bool read_u32(const uint8_t *data, std::size_t size, std::size_t &offset, uint32_t &out)
{
    if (offset + 4 > size) {
        return false;
    }
    out = (static_cast<uint32_t>(data[offset]) << 24) |
          (static_cast<uint32_t>(data[offset + 1]) << 16) |
          (static_cast<uint32_t>(data[offset + 2]) << 8) |
          static_cast<uint32_t>(data[offset + 3]);
    offset += 4;
    return true;
}

bool write_rtcp_header(
    ::yuan::buffer::ByteBuffer &buffer,
    uint8_t count,
    RtcpPacketType packet_type,
    std::size_t body_bytes)
{
    if ((body_bytes % 4) != 0) {
        return false;
    }
    const std::size_t words = body_bytes / 4;
    if (words == 0 || words > 0xFFFF) {
        return false;
    }
    const uint16_t length_field = static_cast<uint16_t>(words);

    const uint8_t b0 = static_cast<uint8_t>((2u << 6) | (count & 0x1Fu));
    buffer.append_u8(b0);
    buffer.append_u8(static_cast<uint8_t>(packet_type));
    buffer.append_u16(length_field);
    return true;
}

} // namespace

bool RtcpReportBlock::serialize(::yuan::buffer::ByteBuffer &buffer) const
{
    if (cumulative_lost < -0x800000 || cumulative_lost > 0x7FFFFF) {
        return false;
    }

    buffer.append_u32(ssrc);
    buffer.append_u8(fraction_lost);
    const uint32_t lost24 = static_cast<uint32_t>(cumulative_lost) & 0x00FFFFFFu;
    append_u24(buffer, lost24);
    buffer.append_u32(highest_sequence_number);
    buffer.append_u32(jitter);
    buffer.append_u32(last_sr);
    buffer.append_u32(delay_since_last_sr);
    return true;
}

bool RtcpReportBlock::deserialize(const uint8_t *data, std::size_t size, std::size_t &offset)
{
    if (offset + 24 > size) {
        return false;
    }

    uint32_t ssrc_net = 0;
    if (!read_u32(data, size, offset, ssrc_net)) {
        return false;
    }
    ssrc = ssrc_net;

    if (!read_u8(data, size, offset, fraction_lost)) {
        return false;
    }

    const uint32_t lost_u24 = decode_u24(data + offset);
    offset += 3;
    if (lost_u24 & 0x00800000u) {
        cumulative_lost = static_cast<int32_t>(lost_u24 | 0xFF000000u);
    } else {
        cumulative_lost = static_cast<int32_t>(lost_u24);
    }

    uint32_t highest_net = 0;
    uint32_t jitter_net = 0;
    uint32_t last_sr_net = 0;
    uint32_t dlsr_net = 0;
    if (!read_u32(data, size, offset, highest_net) ||
        !read_u32(data, size, offset, jitter_net) ||
        !read_u32(data, size, offset, last_sr_net) ||
        !read_u32(data, size, offset, dlsr_net)) {
        return false;
    }

    highest_sequence_number = highest_net;
    jitter = jitter_net;
    last_sr = last_sr_net;
    delay_since_last_sr = dlsr_net;
    return true;
}

bool RtcpPacket::serialize(::yuan::buffer::ByteBuffer &buffer) const
{
    ::yuan::buffer::ByteBuffer body;

    switch (kind) {
    case Kind::sender_report: {
        if (sender_report.report_blocks.size() > 31) {
            return false;
        }
        body.append_u32(sender_report.ssrc);
        body.append_u32(static_cast<uint32_t>(sender_report.ntp_timestamp >> 32));
        body.append_u32(static_cast<uint32_t>(sender_report.ntp_timestamp & 0xFFFFFFFFu));
        body.append_u32(sender_report.rtp_timestamp);
        body.append_u32(sender_report.packet_count);
        body.append_u32(sender_report.octet_count);
        for (const auto &rb : sender_report.report_blocks) {
            if (!rb.serialize(body)) {
                return false;
            }
        }
        if (!write_rtcp_header(
                buffer,
                static_cast<uint8_t>(sender_report.report_blocks.size()),
                RtcpPacketType::sender_report,
                body.readable_bytes())) {
            return false;
        }
        buffer.append(body.read_ptr(), body.readable_bytes());
        return true;
    }
    case Kind::receiver_report: {
        if (receiver_report.report_blocks.size() > 31) {
            return false;
        }
        body.append_u32(receiver_report.ssrc);
        for (const auto &rb : receiver_report.report_blocks) {
            if (!rb.serialize(body)) {
                return false;
            }
        }
        if (!write_rtcp_header(
                buffer,
                static_cast<uint8_t>(receiver_report.report_blocks.size()),
                RtcpPacketType::receiver_report,
                body.readable_bytes())) {
            return false;
        }
        buffer.append(body.read_ptr(), body.readable_bytes());
        return true;
    }
    case Kind::bye: {
        if (bye.sources.empty() || bye.sources.size() > 31) {
            return false;
        }
        for (uint32_t src : bye.sources) {
            body.append_u32(src);
        }
        if (!bye.reason.empty()) {
            if (bye.reason.size() > 255) {
                return false;
            }
            body.append_u8(static_cast<uint8_t>(bye.reason.size()));
            body.append(bye.reason.data(), bye.reason.size());
            const std::size_t pad = (4 - (body.readable_bytes() % 4)) % 4;
            if (pad > 0) {
                std::vector<uint8_t> zeros(pad, 0);
                body.append(zeros.data(), zeros.size());
            }
        }
        if (!write_rtcp_header(
                buffer,
                static_cast<uint8_t>(bye.sources.size()),
                RtcpPacketType::bye,
                body.readable_bytes())) {
            return false;
        }
        buffer.append(body.read_ptr(), body.readable_bytes());
        return true;
    }
    }

    return false;
}

bool RtcpPacket::deserialize(::yuan::buffer::ByteBuffer &buffer)
{
    const auto span = buffer.readable_span();
    const auto *raw = reinterpret_cast<const uint8_t *>(span.data());
    const std::size_t size = span.size();

    if (size < kRtcpHeaderSize) {
        return false;
    }

    std::size_t offset = 0;
    uint8_t b0 = 0;
    uint8_t pt = 0;
    uint16_t length_net = 0;
    if (!read_u8(raw, size, offset, b0) ||
        !read_u8(raw, size, offset, pt) ||
        !read_u16(raw, size, offset, length_net)) {
        return false;
    }

    const uint8_t version = static_cast<uint8_t>((b0 >> 6) & 0x03);
    if (version != 2) {
        return false;
    }
    const uint8_t count = static_cast<uint8_t>(b0 & 0x1F);
    const uint16_t length_words = length_net;
    const std::size_t packet_size = static_cast<std::size_t>(length_words + 1) * 4;
    if (packet_size > size || packet_size < kRtcpHeaderSize) {
        return false;
    }

    const std::size_t body_size = packet_size - kRtcpHeaderSize;
    const std::size_t body_end = packet_size;

    if (pt == static_cast<uint8_t>(RtcpPacketType::sender_report)) {
        kind = Kind::sender_report;
        sender_report = {};
        if (body_size < 24) {
            return false;
        }
        if (count > 31) {
            return false;
        }

        uint32_t ssrc_net = 0;
        uint32_t ntp_hi_net = 0;
        uint32_t ntp_lo_net = 0;
        uint32_t rtp_ts_net = 0;
        uint32_t pkt_cnt_net = 0;
        uint32_t oct_cnt_net = 0;
        if (!read_u32(raw, body_end, offset, ssrc_net) ||
            !read_u32(raw, body_end, offset, ntp_hi_net) ||
            !read_u32(raw, body_end, offset, ntp_lo_net) ||
            !read_u32(raw, body_end, offset, rtp_ts_net) ||
            !read_u32(raw, body_end, offset, pkt_cnt_net) ||
            !read_u32(raw, body_end, offset, oct_cnt_net)) {
            return false;
        }

        sender_report.ssrc = ssrc_net;
        const uint64_t ntp_hi = ntp_hi_net;
        const uint64_t ntp_lo = ntp_lo_net;
        sender_report.ntp_timestamp = (ntp_hi << 32) | ntp_lo;
        sender_report.rtp_timestamp = rtp_ts_net;
        sender_report.packet_count = pkt_cnt_net;
        sender_report.octet_count = oct_cnt_net;

        sender_report.report_blocks.clear();
        sender_report.report_blocks.reserve(count);
        for (uint8_t i = 0; i < count; ++i) {
            RtcpReportBlock rb;
            if (!rb.deserialize(raw, body_end, offset)) {
                return false;
            }
            sender_report.report_blocks.push_back(rb);
        }

        return offset == body_end;
    }

    if (pt == static_cast<uint8_t>(RtcpPacketType::receiver_report)) {
        kind = Kind::receiver_report;
        receiver_report = {};
        if (body_size < 4) {
            return false;
        }

        uint32_t ssrc_net = 0;
        if (!read_u32(raw, body_end, offset, ssrc_net)) {
            return false;
        }
        receiver_report.ssrc = ssrc_net;

        receiver_report.report_blocks.clear();
        receiver_report.report_blocks.reserve(count);
        for (uint8_t i = 0; i < count; ++i) {
            RtcpReportBlock rb;
            if (!rb.deserialize(raw, body_end, offset)) {
                return false;
            }
            receiver_report.report_blocks.push_back(rb);
        }

        return offset == body_end;
    }

    if (pt == static_cast<uint8_t>(RtcpPacketType::bye)) {
        kind = Kind::bye;
        bye = {};

        if (body_size < static_cast<std::size_t>(count) * 4) {
            return false;
        }

        bye.sources.reserve(count);
        for (uint8_t i = 0; i < count; ++i) {
            uint32_t src_net = 0;
            if (!read_u32(raw, body_end, offset, src_net)) {
                return false;
            }
            bye.sources.push_back(src_net);
        }

        if (offset < body_end) {
            uint8_t reason_len = 0;
            if (!read_u8(raw, body_end, offset, reason_len)) {
                return false;
            }
            if (offset + reason_len > body_end) {
                return false;
            }
            bye.reason.assign(reinterpret_cast<const char *>(raw + offset), reason_len);
            offset += reason_len;
        }

        return true;
    }

    return false;
}

} // namespace yuan::net::rtcp
