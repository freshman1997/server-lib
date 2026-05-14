#include "rtc_packet.h"

#include "endian/endian.hpp"

#include <cstddef>

namespace yuan::net::rtc
{

namespace
{

constexpr std::size_t kRtpBaseHeaderSize = 12;

bool read_u8(const uint8_t *data, std::size_t size, std::size_t &offset, uint8_t &out)
{
    if (offset + 1 > size) {
        return false;
    }
    out = data[offset];
    offset += 1;
    return true;
}

bool read_u16(const uint8_t *data, std::size_t size, std::size_t &offset, uint16_t &out)
{
    if (offset + 2 > size) {
        return false;
    }
    uint16_t net = 0;
    net = static_cast<uint16_t>(data[offset] << 8) | static_cast<uint16_t>(data[offset + 1]);
    out = net;
    offset += 2;
    return true;
}

bool read_u32(const uint8_t *data, std::size_t size, std::size_t &offset, uint32_t &out)
{
    if (offset + 4 > size) {
        return false;
    }
    uint32_t net = 0;
    net |= static_cast<uint32_t>(data[offset]) << 24;
    net |= static_cast<uint32_t>(data[offset + 1]) << 16;
    net |= static_cast<uint32_t>(data[offset + 2]) << 8;
    net |= static_cast<uint32_t>(data[offset + 3]);
    out = net;
    offset += 4;
    return true;
}

} // namespace

bool RtcPacket::serialize(::yuan::buffer::ByteBuffer &buffer) const
{
    if (version > 3) {
        return false;
    }
    if (csrc_list.size() > 15) {
        return false;
    }
    if (payload_type > 127) {
        return false;
    }

    const uint8_t cc = static_cast<uint8_t>(csrc_list.size());
    const uint8_t b0 = static_cast<uint8_t>(
        ((version & 0x03) << 6) |
        ((padding ? 1 : 0) << 5) |
        ((extension ? 1 : 0) << 4) |
        (cc & 0x0F));
    const uint8_t b1 = static_cast<uint8_t>(
        ((marker ? 1 : 0) << 7) |
        (payload_type & 0x7F));

    buffer.append_u8(b0);
    buffer.append_u8(b1);
    buffer.append_u16(sequence_number);
    buffer.append_u32(timestamp);
    buffer.append_u32(ssrc);

    for (uint32_t csrc : csrc_list) {
        buffer.append_u32(csrc);
    }

    if (extension) {
        if ((extension_data.size() % 4) != 0) {
            return false;
        }
        const uint16_t ext_words = static_cast<uint16_t>(extension_data.size() / 4);
        buffer.append_u16(extension_profile);
        buffer.append_u16(ext_words);
        if (!extension_data.empty()) {
            buffer.append(extension_data.data(), extension_data.size());
        }
    }

    if (!payload.empty()) {
        buffer.append(payload.data(), payload.size());
    }

    if (padding) {
        if (padding_size == 0) {
            return false;
        }
        if (padding_size > 1) {
            std::vector<uint8_t> zeros(padding_size - 1, 0);
            buffer.append(zeros.data(), zeros.size());
        }
        buffer.append_u8(padding_size);
    }

    return true;
}

bool RtcPacket::deserialize(::yuan::buffer::ByteBuffer &buffer)
{
    const auto span = buffer.readable_span();
    const auto *raw = reinterpret_cast<const uint8_t *>(span.data());
    const std::size_t size = span.size();

    if (size < kRtpBaseHeaderSize) {
        return false;
    }

    std::size_t offset = 0;
    uint8_t b0 = 0;
    uint8_t b1 = 0;
    if (!read_u8(raw, size, offset, b0) || !read_u8(raw, size, offset, b1)) {
        return false;
    }

    version = static_cast<uint8_t>((b0 >> 6) & 0x03);
    padding = ((b0 >> 5) & 0x01) != 0;
    extension = ((b0 >> 4) & 0x01) != 0;
    csrc_count = static_cast<uint8_t>(b0 & 0x0F);
    marker = ((b1 >> 7) & 0x01) != 0;
    payload_type = static_cast<uint8_t>(b1 & 0x7F);

    if (version != 2) {
        return false;
    }

    uint16_t seq_net = 0;
    uint32_t ts_net = 0;
    uint32_t ssrc_net = 0;
    if (!read_u16(raw, size, offset, seq_net) ||
        !read_u32(raw, size, offset, ts_net) ||
        !read_u32(raw, size, offset, ssrc_net)) {
        return false;
    }
    sequence_number = seq_net;
    timestamp = ts_net;
    ssrc = ssrc_net;

    csrc_list.clear();
    csrc_list.reserve(csrc_count);
    for (uint8_t i = 0; i < csrc_count; ++i) {
        uint32_t csrc_net = 0;
        if (!read_u32(raw, size, offset, csrc_net)) {
            return false;
        }
        csrc_list.push_back(csrc_net);
    }

    extension_profile = 0;
    extension_data.clear();
    if (extension) {
        uint16_t profile_net = 0;
        uint16_t words_net = 0;
        if (!read_u16(raw, size, offset, profile_net) || !read_u16(raw, size, offset, words_net)) {
            return false;
        }
        extension_profile = profile_net;
        const uint16_t ext_words = words_net;
        const std::size_t ext_size = static_cast<std::size_t>(ext_words) * 4;
        if (offset + ext_size > size) {
            return false;
        }
        extension_data.assign(raw + offset, raw + offset + ext_size);
        offset += ext_size;
    }

    if (offset > size) {
        return false;
    }

    std::size_t payload_end = size;
    padding_size = 0;
    if (padding) {
        if (size == 0) {
            return false;
        }
        padding_size = raw[size - 1];
        if (padding_size == 0) {
            return false;
        }
        if (padding_size > (size - offset)) {
            return false;
        }
        payload_end = size - padding_size;
    }

    if (payload_end < offset) {
        return false;
    }

    payload.assign(raw + offset, raw + payload_end);
    return true;
}

} // namespace yuan::net::rtc
