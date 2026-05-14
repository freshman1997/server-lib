#include "rtc_packet.h"
#include "rtcp_packet.h"
#include "rtcp_session.h"
#include "rtp_session.h"

#include <cstdint>
#include <iostream>

using namespace yuan::net::rtc;
using namespace yuan::net::rtcp;

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

RtcPacket make_rtp(uint32_t ssrc, uint16_t seq, uint32_t ts)
{
    RtcPacket p;
    p.ssrc = ssrc;
    p.sequence_number = seq;
    p.timestamp = ts;
    p.payload = {0xAB, 0xCD};
    return p;
}

bool test_rtcp_loopback_chain()
{
    const uint32_t remote_ssrc = 0x22223333;
    const uint32_t local_ssrc = 0x11112222;

    RtpSessionManager manager(90000);
    TEST_ASSERT(manager.on_packet_received(make_rtp(remote_ssrc, 5000, 0), 0), "rtp 1 accepted");
    TEST_ASSERT(manager.on_packet_received(make_rtp(remote_ssrc, 5002, 6000), 66), "rtp 2 accepted");
    TEST_ASSERT(manager.on_packet_received(make_rtp(remote_ssrc, 5001, 3000), 33), "rtp 3 accepted");

    auto *session_stats = manager.find_session(remote_ssrc);
    TEST_ASSERT(session_stats != nullptr, "session should exist in manager");
    const auto stats = session_stats->receive_stats();
    TEST_ASSERT(stats.packets_received == 3, "received should be 3");
    TEST_ASSERT(stats.expected_packets == 3, "expected should be 3");

    RtcpSession rtcp_session(local_ssrc);
    rtcp_session.on_sender_activity(0x0102030405060708ull, 77777, 123, 4567);

    const RtcpPacket rr = rtcp_session.build_receiver_report(manager);
    const RtcpPacket sr = rtcp_session.build_sender_report(manager);

    yuan::buffer::ByteBuffer rr_buf;
    yuan::buffer::ByteBuffer sr_buf;
    TEST_ASSERT(rr.serialize(rr_buf), "rr serialize should succeed");
    TEST_ASSERT(sr.serialize(sr_buf), "sr serialize should succeed");

    RtcpPacket rr_parsed;
    RtcpPacket sr_parsed;
    TEST_ASSERT(rr_parsed.deserialize(rr_buf), "rr deserialize should succeed");
    TEST_ASSERT(sr_parsed.deserialize(sr_buf), "sr deserialize should succeed");

    TEST_ASSERT(rr_parsed.kind == RtcpPacket::Kind::receiver_report, "rr kind should match");
    TEST_ASSERT(rr_parsed.receiver_report.ssrc == local_ssrc, "rr local ssrc should match");
    TEST_ASSERT(rr_parsed.receiver_report.report_blocks.size() == 1, "rr report block count should be 1");
    TEST_ASSERT(rr_parsed.receiver_report.report_blocks[0].ssrc == remote_ssrc, "rr remote ssrc should match");

    TEST_ASSERT(sr_parsed.kind == RtcpPacket::Kind::sender_report, "sr kind should match");
    TEST_ASSERT(sr_parsed.sender_report.ssrc == local_ssrc, "sr local ssrc should match");
    TEST_ASSERT(sr_parsed.sender_report.packet_count == 123, "sr packet count should match");
    TEST_ASSERT(sr_parsed.sender_report.octet_count == 4567, "sr octet count should match");
    return true;
}

bool test_rtcp_loopback_multi_stream_with_manager_and_cap()
{
    const uint32_t local_ssrc = 0x51515151;

    RtpSessionManager manager(90000);
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t ssrc = 0x60000000u + i;
        TEST_ASSERT(manager.on_packet_received(make_rtp(ssrc, static_cast<uint16_t>(1000 + i), 3000u * i), 30u * i),
                    "manager should accept stream packet");
    }

    RtcpSession session(local_ssrc);
    session.set_max_report_blocks(4);
    session.set_report_order_policy(RtcpReportOrderPolicy::ssrc_ascending);
    session.on_sender_activity(0x1111222233334444ull, 0x55667788u, 88, 8800);

    const RtcpPacket rr = session.build_receiver_report(manager);
    const RtcpPacket sr = session.build_sender_report(manager);

    TEST_ASSERT(rr.receiver_report.report_blocks.size() == 4, "RR should honor cap 4");
    TEST_ASSERT(sr.sender_report.report_blocks.size() == 4, "SR should honor cap 4");

    TEST_ASSERT(rr.receiver_report.report_blocks[0].ssrc == 0x60000000u, "RR order should start from smallest SSRC");
    TEST_ASSERT(rr.receiver_report.report_blocks[3].ssrc == 0x60000003u, "RR order should follow ascending SSRC");
    TEST_ASSERT(sr.sender_report.report_blocks[0].ssrc == 0x60000000u, "SR order should start from smallest SSRC");
    TEST_ASSERT(sr.sender_report.report_blocks[3].ssrc == 0x60000003u, "SR order should follow ascending SSRC");

    yuan::buffer::ByteBuffer rr_buf;
    yuan::buffer::ByteBuffer sr_buf;
    TEST_ASSERT(rr.serialize(rr_buf), "RR serialize should succeed");
    TEST_ASSERT(sr.serialize(sr_buf), "SR serialize should succeed");

    RtcpPacket rr_parsed;
    RtcpPacket sr_parsed;
    TEST_ASSERT(rr_parsed.deserialize(rr_buf), "RR deserialize should succeed");
    TEST_ASSERT(sr_parsed.deserialize(sr_buf), "SR deserialize should succeed");

    TEST_ASSERT(rr_parsed.kind == RtcpPacket::Kind::receiver_report, "parsed RR kind should match");
    TEST_ASSERT(rr_parsed.receiver_report.ssrc == local_ssrc, "parsed RR local SSRC should match");
    TEST_ASSERT(rr_parsed.receiver_report.report_blocks.size() == 4, "parsed RR block count should stay capped at 4");

    TEST_ASSERT(sr_parsed.kind == RtcpPacket::Kind::sender_report, "parsed SR kind should match");
    TEST_ASSERT(sr_parsed.sender_report.ssrc == local_ssrc, "parsed SR local SSRC should match");
    TEST_ASSERT(sr_parsed.sender_report.packet_count == 88, "parsed SR packet_count should match");
    TEST_ASSERT(sr_parsed.sender_report.octet_count == 8800, "parsed SR octet_count should match");
    TEST_ASSERT(sr_parsed.sender_report.report_blocks.size() == 4, "parsed SR block count should stay capped at 4");

    for (std::size_t i = 0; i < 4; ++i) {
        const uint32_t expected_ssrc = 0x60000000u + static_cast<uint32_t>(i);
        TEST_ASSERT(rr_parsed.receiver_report.report_blocks[i].ssrc == expected_ssrc, "parsed RR block SSRC should keep sorted order");
        TEST_ASSERT(sr_parsed.sender_report.report_blocks[i].ssrc == expected_ssrc, "parsed SR block SSRC should keep sorted order");
    }

    return true;
}

bool test_rtcp_loopback_multi_stream_multi_round_fraction_lost_stress()
{
    const uint32_t local_ssrc = 0x71717171;
    const uint32_t base_ssrc = 0x72000000u;

    RtpSessionManager manager(90000);
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t ssrc = base_ssrc + i;
        TEST_ASSERT(manager.on_packet_received(make_rtp(ssrc, static_cast<uint16_t>(1000 + i), 3000u * i), 10u * i),
                    "round1 packet should be accepted");
    }

    RtcpSession session(local_ssrc);
    session.set_max_report_blocks(8);
    session.set_report_order_policy(RtcpReportOrderPolicy::ssrc_ascending);

    const RtcpPacket rr1 = session.build_receiver_report(manager);
    TEST_ASSERT(rr1.receiver_report.report_blocks.size() == 8, "round1 RR should include all 8 streams");
    for (std::size_t i = 0; i < 8; ++i) {
        const uint32_t expected_ssrc = base_ssrc + static_cast<uint32_t>(i);
        TEST_ASSERT(rr1.receiver_report.report_blocks[i].ssrc == expected_ssrc, "round1 RR SSRC order should be ascending");
        TEST_ASSERT(rr1.receiver_report.report_blocks[i].fraction_lost == 0, "round1 fraction_lost should be 0 baseline");
    }

    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t ssrc = base_ssrc + i;
        const uint16_t seq = static_cast<uint16_t>(1001 + i + (i % 2 == 0 ? 1 : 0));
        TEST_ASSERT(manager.on_packet_received(make_rtp(ssrc, seq, 50000u + 3000u * i), 100u + 10u * i),
                    "round2 packet should be accepted");
    }

    const RtcpPacket rr2 = session.build_receiver_report(manager);
    TEST_ASSERT(rr2.receiver_report.report_blocks.size() == 8, "round2 RR should include all 8 streams");
    for (std::size_t i = 0; i < 8; ++i) {
        const uint8_t expected_fraction = (i % 2 == 0) ? 128 : 0;
        TEST_ASSERT(rr2.receiver_report.report_blocks[i].fraction_lost == expected_fraction,
                    "round2 fraction_lost should match gap pattern");
    }

    for (uint32_t i = 0; i < 8; ++i) {
        if (i % 2 != 0) {
            continue;
        }
        const uint32_t ssrc = base_ssrc + i;
        const uint16_t late_seq = static_cast<uint16_t>(1001 + i);
        TEST_ASSERT(manager.on_packet_received(make_rtp(ssrc, late_seq, 80000u + 3000u * i), 170u + 10u * i),
                    "round3 late recovery packet should be accepted");
    }

    const RtcpPacket rr3 = session.build_receiver_report(manager);
    TEST_ASSERT(rr3.receiver_report.report_blocks.size() == 8, "round3 RR should include all 8 streams");
    for (std::size_t i = 0; i < 8; ++i) {
        TEST_ASSERT(rr3.receiver_report.report_blocks[i].fraction_lost == 0,
                    "round3 fraction_lost should be 0 after no new expected loss window");
    }

    return true;
}

} // namespace

int main()
{
    std::cout << "=== RTCP Loopback Test Suite ===\n";
    RUN_TEST(test_rtcp_loopback_chain);
    RUN_TEST(test_rtcp_loopback_multi_stream_with_manager_and_cap);
    RUN_TEST(test_rtcp_loopback_multi_stream_multi_round_fraction_lost_stress);

    std::cout << "Result: " << g_pass << "/" << g_run << " passed";
    if (g_fail > 0) {
        std::cout << ", " << g_fail << " failed";
    }
    std::cout << "\n";
    return g_fail == 0 ? 0 : 1;
}
