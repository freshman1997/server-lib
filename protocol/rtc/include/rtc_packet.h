#ifndef __NET_RTC_PACKET_H__
#define __NET_RTC_PACKET_H__

#include "buffer/byte_buffer.h"

#include <cstdint>
#include <vector>

namespace yuan::net::rtc
{

struct RtcPacket
{
    uint8_t version = 2;
    bool padding = false;
    bool extension = false;
    uint8_t csrc_count = 0;
    bool marker = false;
    uint8_t payload_type = 96;
    uint16_t sequence_number = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;

    std::vector<uint32_t> csrc_list;

    uint16_t extension_profile = 0;
    std::vector<uint8_t> extension_data;

    std::vector<uint8_t> payload;
    uint8_t padding_size = 0;

    bool serialize(::yuan::buffer::ByteBuffer &buffer) const;
    bool deserialize(::yuan::buffer::ByteBuffer &buffer);
};

} // namespace yuan::net::rtc

#endif
