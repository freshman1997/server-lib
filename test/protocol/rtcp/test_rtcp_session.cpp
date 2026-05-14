#include "rtcp_session.h"

#include <cstdint>
#include <iostream>
#include <vector>

using namespace yuan::net::rtcp;
using namespace yuan::net::rtc;

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

bool test_rtcp_session_build_rr()
{
    RtpReceiveStats stats;
    stats.ssrc = 0x01020304;
    stats.initialized = true;
    stats.expected_packets = 10;
    stats.packets_received = 8;
    stats.cumulative_lost = 2;
    stats.highest_sequence_number = 56789;
    stats.jitter = 123;

    RtcpSession session(0xAABBCCDD);
    const RtcpPacket rr = session.build_receiver_report(stats);

    TEST_ASSERT(rr.kind == RtcpPacket::Kind::receiver_report, "kind should be RR");
    TEST_ASSERT(rr.receiver_report.ssrc == 0xAABBCCDD, "local ssrc should match");
    TEST_ASSERT(rr.receiver_report.report_blocks.size() == 1, "one report block expected");
    TEST_ASSERT(rr.receiver_report.report_blocks[0].ssrc == stats.ssrc, "remote ssrc should match");
    TEST_ASSERT(rr.receiver_report.report_blocks[0].cumulative_lost == 2, "lost should match");
    TEST_ASSERT(rr.receiver_report.report_blocks[0].jitter == 123, "jitter should match");
    TEST_ASSERT(rr.receiver_report.report_blocks[0].fraction_lost == 0, "first RR interval fraction_lost should be 0");

    stats.expected_packets = 18;
    stats.packets_received = 14;
    const RtcpPacket rr2 = session.build_receiver_report(stats);
    TEST_ASSERT(rr2.receiver_report.report_blocks.size() == 1, "one report block expected in second RR");
    TEST_ASSERT(rr2.receiver_report.report_blocks[0].fraction_lost == 64, "second RR interval fraction_lost should be 64");
    return true;
}

bool test_rtcp_session_build_sr()
{
    RtpReceiveStats stats;
    stats.ssrc = 0x0A0B0C0D;
    stats.highest_sequence_number = 100;
    stats.cumulative_lost = 0;
    stats.jitter = 7;

    RtcpSession session(0x01010101);
    session.on_sender_activity(0x1122334455667788ull, 123456, 500, 64000);
    const RtcpPacket sr = session.build_sender_report(stats);

    TEST_ASSERT(sr.kind == RtcpPacket::Kind::sender_report, "kind should be SR");
    TEST_ASSERT(sr.sender_report.ssrc == 0x01010101, "local ssrc should match");
    TEST_ASSERT(sr.sender_report.ntp_timestamp == 0x1122334455667788ull, "ntp should match");
    TEST_ASSERT(sr.sender_report.rtp_timestamp == 123456, "rtp timestamp should match");
    TEST_ASSERT(sr.sender_report.packet_count == 500, "packet_count should match");
    TEST_ASSERT(sr.sender_report.octet_count == 64000, "octet_count should match");
    TEST_ASSERT(sr.sender_report.report_blocks.size() == 1, "one report block expected");
    return true;
}

bool test_rtcp_session_build_rr_multi_stream()
{
    RtpReceiveStats a;
    a.ssrc = 0x01010101;
    a.initialized = true;
    a.expected_packets = 100;
    a.packets_received = 90;
    a.highest_sequence_number = 1000;
    a.jitter = 11;

    RtpReceiveStats b;
    b.ssrc = 0x02020202;
    b.initialized = true;
    b.expected_packets = 80;
    b.packets_received = 79;
    b.highest_sequence_number = 2000;
    b.jitter = 22;

    RtcpSession session(0x0F0F0F0F);
    const RtcpPacket rr1 = session.build_receiver_report(std::vector<RtpReceiveStats>{a, b});
    TEST_ASSERT(rr1.kind == RtcpPacket::Kind::receiver_report, "kind should be RR");
    TEST_ASSERT(rr1.receiver_report.report_blocks.size() == 2, "two report blocks expected");
    TEST_ASSERT(rr1.receiver_report.report_blocks[0].fraction_lost == 0, "first interval A fraction_lost should be 0");
    TEST_ASSERT(rr1.receiver_report.report_blocks[1].fraction_lost == 0, "first interval B fraction_lost should be 0");

    a.expected_packets = 120;
    a.packets_received = 100;
    b.expected_packets = 100;
    b.packets_received = 90;
    const RtcpPacket rr2 = session.build_receiver_report(std::vector<RtpReceiveStats>{a, b});
    TEST_ASSERT(rr2.receiver_report.report_blocks.size() == 2, "two report blocks expected in second RR");
    TEST_ASSERT(rr2.receiver_report.report_blocks[0].fraction_lost == 128, "A second interval fraction_lost should be 128");
    TEST_ASSERT(rr2.receiver_report.report_blocks[1].fraction_lost == 115, "B second interval fraction_lost should be 115");
    return true;
}

bool test_rtcp_session_build_sr_multi_stream()
{
    RtpReceiveStats a;
    a.ssrc = 0x11111111;
    a.initialized = true;
    a.expected_packets = 10;
    a.packets_received = 9;
    a.highest_sequence_number = 123;
    a.jitter = 5;

    RtpReceiveStats b;
    b.ssrc = 0x22222222;
    b.initialized = true;
    b.expected_packets = 20;
    b.packets_received = 20;
    b.highest_sequence_number = 456;
    b.jitter = 7;

    RtcpSession session(0x33333333);
    session.on_sender_activity(0x0102030405060708ull, 9999, 77, 8888);
    const RtcpPacket sr = session.build_sender_report(std::vector<RtpReceiveStats>{a, b});
    TEST_ASSERT(sr.kind == RtcpPacket::Kind::sender_report, "kind should be SR");
    TEST_ASSERT(sr.sender_report.report_blocks.size() == 2, "two report blocks expected");
    TEST_ASSERT(sr.sender_report.report_blocks[0].ssrc == 0x11111111, "first report block ssrc should match");
    TEST_ASSERT(sr.sender_report.report_blocks[1].ssrc == 0x22222222, "second report block ssrc should match");
    return true;
}

bool test_rtcp_session_manager_adapter_and_block_limit()
{
    RtpSessionManager manager(90000);
    for (uint32_t i = 0; i < 40; ++i) {
        const uint32_t ssrc = 0x10000000u + i;
        RtcPacket pkt;
        pkt.ssrc = ssrc;
        pkt.sequence_number = static_cast<uint16_t>(100 + i);
        pkt.timestamp = 3000u * i;
        pkt.payload = {0xAA};
        TEST_ASSERT(manager.on_packet_received(pkt, 33u * i), "manager should accept packet");
    }

    RtcpSession session(0x77777777);
    const RtcpPacket rr = session.build_receiver_report(manager);
    const RtcpPacket sr = session.build_sender_report(manager);
    TEST_ASSERT(rr.kind == RtcpPacket::Kind::receiver_report, "rr kind should match");
    TEST_ASSERT(sr.kind == RtcpPacket::Kind::sender_report, "sr kind should match");
    TEST_ASSERT(rr.receiver_report.report_blocks.size() == 31, "RR report blocks should be capped at 31");
    TEST_ASSERT(sr.sender_report.report_blocks.size() == 31, "SR report blocks should be capped at 31");
    return true;
}

bool test_rtcp_session_custom_block_limit()
{
    RtpSessionManager manager(90000);
    for (uint32_t i = 0; i < 12; ++i) {
        const uint32_t ssrc = 0x20000000u + i;
        RtcPacket pkt;
        pkt.ssrc = ssrc;
        pkt.sequence_number = static_cast<uint16_t>(200 + i);
        pkt.timestamp = 9000u + 3000u * i;
        pkt.payload = {0x55};
        TEST_ASSERT(manager.on_packet_received(pkt, 100u + 10u * i), "manager should accept packet");
    }

    RtcpSession session(0x88888888);
    session.set_max_report_blocks(5);

    const RtcpPacket rr = session.build_receiver_report(manager);
    const RtcpPacket sr = session.build_sender_report(manager);
    TEST_ASSERT(rr.receiver_report.report_blocks.size() == 5, "RR should respect custom cap 5");
    TEST_ASSERT(sr.sender_report.report_blocks.size() == 5, "SR should respect custom cap 5");
    return true;
}

bool test_rtcp_session_report_order_policy_ssrc_ascending()
{
    RtpReceiveStats a;
    a.ssrc = 0x30000003;
    a.initialized = true;
    a.expected_packets = 10;
    a.packets_received = 10;

    RtpReceiveStats b;
    b.ssrc = 0x30000001;
    b.initialized = true;
    b.expected_packets = 10;
    b.packets_received = 9;

    RtpReceiveStats c;
    c.ssrc = 0x30000002;
    c.initialized = true;
    c.expected_packets = 10;
    c.packets_received = 8;

    std::vector<RtpReceiveStats> input{a, b, c};

    RtcpSession session(0x90909090);
    session.set_report_order_policy(RtcpReportOrderPolicy::ssrc_ascending);

    const RtcpPacket rr = session.build_receiver_report(input);
    const RtcpPacket sr = session.build_sender_report(input);

    TEST_ASSERT(rr.receiver_report.report_blocks.size() == 3, "RR should have 3 blocks");
    TEST_ASSERT(sr.sender_report.report_blocks.size() == 3, "SR should have 3 blocks");

    TEST_ASSERT(rr.receiver_report.report_blocks[0].ssrc == 0x30000001, "RR block 0 should be smallest SSRC");
    TEST_ASSERT(rr.receiver_report.report_blocks[1].ssrc == 0x30000002, "RR block 1 should be middle SSRC");
    TEST_ASSERT(rr.receiver_report.report_blocks[2].ssrc == 0x30000003, "RR block 2 should be largest SSRC");

    TEST_ASSERT(sr.sender_report.report_blocks[0].ssrc == 0x30000001, "SR block 0 should be smallest SSRC");
    TEST_ASSERT(sr.sender_report.report_blocks[1].ssrc == 0x30000002, "SR block 1 should be middle SSRC");
    TEST_ASSERT(sr.sender_report.report_blocks[2].ssrc == 0x30000003, "SR block 2 should be largest SSRC");
    return true;
}

bool test_rtcp_session_report_order_tie_break_input_index()
{
    RtpReceiveStats a;
    a.ssrc = 0x44444444;
    a.initialized = true;
    a.expected_packets = 10;
    a.packets_received = 10;
    a.jitter = 10;

    RtpReceiveStats b;
    b.ssrc = 0x44444444;
    b.initialized = true;
    b.expected_packets = 10;
    b.packets_received = 9;
    b.jitter = 20;

    RtpReceiveStats c;
    c.ssrc = 0x44444444;
    c.initialized = true;
    c.expected_packets = 10;
    c.packets_received = 8;
    c.jitter = 30;

    std::vector<RtpReceiveStats> input{b, c, a};

    RtcpSession session(0x93939393);
    session.set_report_order_policy(RtcpReportOrderPolicy::ssrc_ascending);

    const RtcpPacket rr = session.build_receiver_report(input);
    const RtcpPacket sr = session.build_sender_report(input);

    TEST_ASSERT(rr.receiver_report.report_blocks.size() == 3, "RR should have 3 blocks");
    TEST_ASSERT(sr.sender_report.report_blocks.size() == 3, "SR should have 3 blocks");
    TEST_ASSERT(rr.receiver_report.report_blocks[0].jitter == 20, "RR tie-break should keep input order index 0");
    TEST_ASSERT(rr.receiver_report.report_blocks[1].jitter == 30, "RR tie-break should keep input order index 1");
    TEST_ASSERT(rr.receiver_report.report_blocks[2].jitter == 10, "RR tie-break should keep input order index 2");
    TEST_ASSERT(sr.sender_report.report_blocks[0].jitter == 20, "SR tie-break should keep input order index 0");
    TEST_ASSERT(sr.sender_report.report_blocks[1].jitter == 30, "SR tie-break should keep input order index 1");
    TEST_ASSERT(sr.sender_report.report_blocks[2].jitter == 10, "SR tie-break should keep input order index 2");
    return true;
}

bool test_rtcp_fraction_lost_late_packet_recovery_regression()
{
    RtcpSession session(0xA0A0A0A0);

    RtpReceiveStats first;
    first.ssrc = 0x51515151;
    first.initialized = true;
    first.expected_packets = 100;
    first.packets_received = 90;

    RtpReceiveStats second;
    second.ssrc = 0x51515151;
    second.initialized = true;
    second.expected_packets = 110;
    second.packets_received = 99;

    RtpReceiveStats recovered;
    recovered.ssrc = 0x51515151;
    recovered.initialized = true;
    recovered.expected_packets = 111;
    recovered.packets_received = 101;

    const RtcpPacket rr1 = session.build_receiver_report(first);
    const RtcpPacket rr2 = session.build_receiver_report(second);
    const RtcpPacket rr3 = session.build_receiver_report(recovered);

    TEST_ASSERT(rr1.receiver_report.report_blocks[0].fraction_lost == 0, "first RR interval should be 0");
    TEST_ASSERT(rr2.receiver_report.report_blocks[0].fraction_lost == 25, "second RR interval should be 25");
    TEST_ASSERT(rr3.receiver_report.report_blocks[0].fraction_lost == 0, "late packet recovery should clamp fraction to 0");
    return true;
}

bool test_rtcp_snapshot_reset_interval_resets_fraction_lost_baseline()
{
    RtcpSession session(0xB0B0B0B0);
    session.set_snapshot_reset_interval(2);

    RtpReceiveStats first;
    first.ssrc = 0x62626262;
    first.initialized = true;
    first.expected_packets = 100;
    first.packets_received = 90;

    RtpReceiveStats second;
    second.ssrc = 0x62626262;
    second.initialized = true;
    second.expected_packets = 110;
    second.packets_received = 95;

    RtpReceiveStats third;
    third.ssrc = 0x62626262;
    third.initialized = true;
    third.expected_packets = 120;
    third.packets_received = 100;

    const RtcpPacket rr1 = session.build_receiver_report(first);
    const RtcpPacket rr2 = session.build_receiver_report(second);
    const RtcpPacket rr3 = session.build_receiver_report(third);

    TEST_ASSERT(rr1.receiver_report.report_blocks[0].fraction_lost == 0, "first RR interval should be 0");
    TEST_ASSERT(rr2.receiver_report.report_blocks[0].fraction_lost == 128, "second RR interval should be 128");
    TEST_ASSERT(rr3.receiver_report.report_blocks[0].fraction_lost == 0, "RR after snapshot reset should restart baseline");
    return true;
}

bool test_rtcp_session_last_sr_and_delay_fields()
{
    RtcpSession session(0xC0C0C0C0);
    const uint64_t sr_ntp = 0x1020304050607080ull;
    const uint64_t sr_arrival_ms = 5000;
    session.on_sender_activity(sr_ntp, 1234, 7, 700, sr_arrival_ms);

    RtpReceiveStats stats;
    stats.ssrc = 0x73737373;
    stats.initialized = true;
    stats.expected_packets = 10;
    stats.packets_received = 9;
    stats.highest_sequence_number = 777;
    stats.jitter = 3;

    const RtcpPacket rr = session.build_receiver_report(stats);
    TEST_ASSERT(rr.receiver_report.report_blocks.size() == 1, "RR should have one block");
    const auto &block = rr.receiver_report.report_blocks[0];
    TEST_ASSERT(block.last_sr == static_cast<uint32_t>((sr_ntp >> 16) & 0xFFFFFFFFull),
                "last_sr should be middle 32 bits of sender NTP");
    TEST_ASSERT(block.delay_since_last_sr > 0, "delay since last sr should be positive");

    const auto snap = session.stats_snapshot();
    TEST_ASSERT(snap.has_sender_activity, "stats should indicate sender activity");
    TEST_ASSERT(snap.last_sr_lsr == block.last_sr, "snapshot lsr should match report block");
    TEST_ASSERT(snap.last_sr_delay_65536 == block.delay_since_last_sr, "snapshot dlsr should match report block");
    TEST_ASSERT(snap.rr_reports_built >= 1, "rr built counter should increase");

    RtcpPacket sr;
    sr = session.build_sender_report(stats);
    TEST_ASSERT(sr.kind == RtcpPacket::Kind::sender_report, "sender report should build");
    const auto snap_after_sr = session.stats_snapshot();
    TEST_ASSERT(snap_after_sr.sr_reports_built >= 1, "sr built counter should increase");
    return true;
}

} // namespace

int main()
{
    std::cout << "=== RTCP Session Test Suite ===\n";
    RUN_TEST(test_rtcp_session_build_rr);
    RUN_TEST(test_rtcp_session_build_sr);
    RUN_TEST(test_rtcp_session_build_rr_multi_stream);
    RUN_TEST(test_rtcp_session_build_sr_multi_stream);
    RUN_TEST(test_rtcp_session_manager_adapter_and_block_limit);
    RUN_TEST(test_rtcp_session_custom_block_limit);
    RUN_TEST(test_rtcp_session_report_order_policy_ssrc_ascending);
    RUN_TEST(test_rtcp_session_report_order_tie_break_input_index);
    RUN_TEST(test_rtcp_fraction_lost_late_packet_recovery_regression);
    RUN_TEST(test_rtcp_snapshot_reset_interval_resets_fraction_lost_baseline);
    RUN_TEST(test_rtcp_session_last_sr_and_delay_fields);

    std::cout << "Result: " << g_pass << "/" << g_run << " passed";
    if (g_fail > 0) {
        std::cout << ", " << g_fail << " failed";
    }
    std::cout << "\n";
    return g_fail == 0 ? 0 : 1;
}
