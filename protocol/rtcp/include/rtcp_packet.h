#ifndef __NET_RTCP_PACKET_H__
#define __NET_RTCP_PACKET_H__

#include "buffer/byte_buffer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::rtcp
{

enum class RtcpPacketType : uint8_t
{
    sender_report = 200,
    receiver_report = 201,
    source_description = 202,
    bye = 203
};

struct RtcpReportBlock
{
    uint32_t ssrc = 0;
    uint8_t fraction_lost = 0;
    int32_t cumulative_lost = 0;
    uint32_t highest_sequence_number = 0;
    uint32_t jitter = 0;
    uint32_t last_sr = 0;
    uint32_t delay_since_last_sr = 0;

    bool serialize(::yuan::buffer::ByteBuffer &buffer) const;
    bool deserialize(const uint8_t *data, std::size_t size, std::size_t &offset);
};

struct RtcpSenderReport
{
    uint32_t ssrc = 0;
    uint64_t ntp_timestamp = 0;
    uint32_t rtp_timestamp = 0;
    uint32_t packet_count = 0;
    uint32_t octet_count = 0;
    std::vector<RtcpReportBlock> report_blocks;
};

struct RtcpReceiverReport
{
    uint32_t ssrc = 0;
    std::vector<RtcpReportBlock> report_blocks;
};

struct RtcpBye
{
    std::vector<uint32_t> sources;
    std::string reason;
};

class RtcpPacket
{
public:
    enum class Kind
    {
        sender_report,
        receiver_report,
        bye
    };

    Kind kind = Kind::receiver_report;
    RtcpSenderReport sender_report;
    RtcpReceiverReport receiver_report;
    RtcpBye bye;

    bool serialize(::yuan::buffer::ByteBuffer &buffer) const;
    bool deserialize(::yuan::buffer::ByteBuffer &buffer);
};

} // namespace yuan::net::rtcp

#endif
