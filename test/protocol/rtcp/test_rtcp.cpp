#include "rtcp_packet.h"

#include "buffer/byte_buffer.h"

#include <cstdint>
#include <iostream>

using namespace yuan::net::rtcp;
using namespace yuan::buffer;

namespace
{

int g_run = 0;
int g_pass = 0;
int g_fail = 0;

#define TEST_ASSERT(expr, msg)                                                                  \
    do {                                                                                         \
        if (!(expr)) {                                                                           \
            std::cout << "  FAIL: " << msg << " (line " << __LINE__ << ")\n";             \
            return false;                                                                        \
        }                                                                                        \
    } while (0)

#define RUN_TEST(func)                                                                           \
    do {                                                                                         \
        ++g_run;                                                                                 \
        std::cout << "Running " #func "...\n";                                               \
        if (func()) {                                                                            \
            ++g_pass;                                                                            \
            std::cout << "  PASS\n";                                                           \
        } else {                                                                                 \
            ++g_fail;                                                                            \
            std::cout << "  FAIL\n";                                                           \
        }                                                                                        \
    } while (0)

RtcpReportBlock make_block(uint32_t ssrc)
{
    RtcpReportBlock b;
    b.ssrc = ssrc;
    b.fraction_lost = 10;
    b.cumulative_lost = -12;
    b.highest_sequence_number = 1000;
    b.jitter = 55;
    b.last_sr = 1234;
    b.delay_since_last_sr = 4321;
    return b;
}

bool test_rtcp_sr_roundtrip()
{
    RtcpPacket packet;
    packet.kind = RtcpPacket::Kind::sender_report;
    packet.sender_report.ssrc = 0x11111111;
    packet.sender_report.ntp_timestamp = 0x1234567890ABCDEFull;
    packet.sender_report.rtp_timestamp = 0x87654321;
    packet.sender_report.packet_count = 1024;
    packet.sender_report.octet_count = 4096;
    packet.sender_report.report_blocks.push_back(make_block(0x22222222));

    ByteBuffer buf;
    TEST_ASSERT(packet.serialize(buf), "serialize should succeed");

    RtcpPacket parsed;
    TEST_ASSERT(parsed.deserialize(buf), "deserialize should succeed");
    TEST_ASSERT(parsed.kind == RtcpPacket::Kind::sender_report, "kind should be sender_report");
    TEST_ASSERT(parsed.sender_report.ssrc == packet.sender_report.ssrc, "ssrc should match");
    TEST_ASSERT(parsed.sender_report.ntp_timestamp == packet.sender_report.ntp_timestamp, "ntp should match");
    TEST_ASSERT(parsed.sender_report.report_blocks.size() == 1, "report block count should be 1");
    TEST_ASSERT(parsed.sender_report.report_blocks[0].cumulative_lost == -12, "cumulative lost should match");
    return true;
}

bool test_rtcp_rr_roundtrip()
{
    RtcpPacket packet;
    packet.kind = RtcpPacket::Kind::receiver_report;
    packet.receiver_report.ssrc = 0xAABBCCDD;
    packet.receiver_report.report_blocks.push_back(make_block(0x10000001));
    packet.receiver_report.report_blocks.push_back(make_block(0x10000002));

    ByteBuffer buf;
    TEST_ASSERT(packet.serialize(buf), "serialize should succeed");

    RtcpPacket parsed;
    TEST_ASSERT(parsed.deserialize(buf), "deserialize should succeed");
    TEST_ASSERT(parsed.kind == RtcpPacket::Kind::receiver_report, "kind should be receiver_report");
    TEST_ASSERT(parsed.receiver_report.ssrc == packet.receiver_report.ssrc, "ssrc should match");
    TEST_ASSERT(parsed.receiver_report.report_blocks.size() == 2, "report block count should be 2");
    return true;
}

bool test_rtcp_bye_roundtrip()
{
    RtcpPacket packet;
    packet.kind = RtcpPacket::Kind::bye;
    packet.bye.sources = {0x01020304, 0x05060708};
    packet.bye.reason = "normal shutdown";

    ByteBuffer buf;
    TEST_ASSERT(packet.serialize(buf), "serialize should succeed");

    RtcpPacket parsed;
    TEST_ASSERT(parsed.deserialize(buf), "deserialize should succeed");
    TEST_ASSERT(parsed.kind == RtcpPacket::Kind::bye, "kind should be bye");
    TEST_ASSERT(parsed.bye.sources.size() == 2, "source count should be 2");
    TEST_ASSERT(parsed.bye.reason == "normal shutdown", "reason should match");
    return true;
}

} // namespace

int main()
{
    std::cout << "=== RTCP Protocol Test Suite ===\n";
    RUN_TEST(test_rtcp_sr_roundtrip);
    RUN_TEST(test_rtcp_rr_roundtrip);
    RUN_TEST(test_rtcp_bye_roundtrip);

    std::cout << "Result: " << g_pass << "/" << g_run << " passed";
    if (g_fail > 0) {
        std::cout << ", " << g_fail << " failed";
    }
    std::cout << "\n";
    return g_fail == 0 ? 0 : 1;
}
