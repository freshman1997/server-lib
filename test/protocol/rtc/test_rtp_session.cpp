#include "rtp_session.h"

#include <cstdint>
#include <iostream>

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

RtcPacket make_packet(uint32_t ssrc, uint16_t seq, uint32_t ts)
{
    RtcPacket p;
    p.ssrc = ssrc;
    p.sequence_number = seq;
    p.timestamp = ts;
    p.payload = {0x11, 0x22};
    return p;
}

bool test_rtp_session_basic_stats()
{
    RtpSession session(0xABCDEF01, 90000);
    TEST_ASSERT(session.on_packet_received(make_packet(0xABCDEF01, 1000, 10000), 1000), "packet 1 accepted");
    TEST_ASSERT(session.on_packet_received(make_packet(0xABCDEF01, 1001, 13000), 1033), "packet 2 accepted");
    TEST_ASSERT(session.on_packet_received(make_packet(0xABCDEF01, 1002, 16000), 1066), "packet 3 accepted");

    const auto stats = session.receive_stats();
    TEST_ASSERT(stats.initialized, "stats should be initialized");
    TEST_ASSERT(stats.packets_received == 3, "received should be 3");
    TEST_ASSERT(stats.expected_packets == 3, "expected should be 3");
    TEST_ASSERT(stats.cumulative_lost == 0, "lost should be 0");
    TEST_ASSERT(stats.highest_sequence_number >= 1002, "highest seq should be >= 1002");
    return true;
}

bool test_rtp_session_loss_estimation()
{
    RtpSession session(0x01020304, 90000);
    TEST_ASSERT(session.on_packet_received(make_packet(0x01020304, 2000, 0), 0), "packet 1 accepted");
    TEST_ASSERT(session.on_packet_received(make_packet(0x01020304, 2002, 6000), 66), "packet 2 accepted");

    const auto stats = session.receive_stats();
    TEST_ASSERT(stats.packets_received == 2, "received should be 2");
    TEST_ASSERT(stats.expected_packets == 3, "expected should be 3 due to gap");
    TEST_ASSERT(stats.cumulative_lost == 1, "lost should be 1");
    return true;
}

bool test_rtp_session_reject_wrong_ssrc()
{
    RtpSession session(0x11111111, 90000);
    TEST_ASSERT(session.on_packet_received(make_packet(0x11111111, 1, 0), 0), "packet with correct ssrc accepted");
    TEST_ASSERT(!session.on_packet_received(make_packet(0x22222222, 2, 3000), 33), "packet with wrong ssrc rejected");
    return true;
}

bool test_rtp_session_out_of_order_does_not_increase_expected()
{
    RtpSession session(0x33333333, 90000);
    TEST_ASSERT(session.on_packet_received(make_packet(0x33333333, 100, 0), 0), "packet 100 accepted");
    TEST_ASSERT(session.on_packet_received(make_packet(0x33333333, 102, 6000), 66), "packet 102 accepted");
    TEST_ASSERT(session.on_packet_received(make_packet(0x33333333, 101, 3000), 33), "late packet 101 accepted");

    const auto stats = session.receive_stats();
    TEST_ASSERT(stats.expected_packets == 3, "expected should remain 3");
    TEST_ASSERT(stats.packets_received == 3, "received should be 3");
    TEST_ASSERT(stats.cumulative_lost == 0, "lost should be 0 after late recovery");
    return true;
}

bool test_rtp_session_wrap_around()
{
    RtpSession session(0x44444444, 90000);
    TEST_ASSERT(session.on_packet_received(make_packet(0x44444444, 65534, 0), 0), "packet 65534 accepted");
    TEST_ASSERT(session.on_packet_received(make_packet(0x44444444, 65535, 3000), 33), "packet 65535 accepted");
    TEST_ASSERT(session.on_packet_received(make_packet(0x44444444, 0, 6000), 66), "packet 0 accepted");
    TEST_ASSERT(session.on_packet_received(make_packet(0x44444444, 1, 9000), 99), "packet 1 accepted");

    const auto stats = session.receive_stats();
    TEST_ASSERT(stats.expected_packets == 4, "expected should be 4 across wrap");
    TEST_ASSERT(stats.packets_received == 4, "received should be 4 across wrap");
    TEST_ASSERT(stats.cumulative_lost == 0, "lost should be 0 across wrap");
    TEST_ASSERT(stats.highest_sequence_number >= 65537u, "extended highest sequence should cross cycle");
    return true;
}

} // namespace

int main()
{
    std::cout << "=== RTP Session Test Suite ===\n";
    RUN_TEST(test_rtp_session_basic_stats);
    RUN_TEST(test_rtp_session_loss_estimation);
    RUN_TEST(test_rtp_session_reject_wrong_ssrc);
    RUN_TEST(test_rtp_session_out_of_order_does_not_increase_expected);
    RUN_TEST(test_rtp_session_wrap_around);

    std::cout << "Result: " << g_pass << "/" << g_run << " passed";
    if (g_fail > 0) {
        std::cout << ", " << g_fail << " failed";
    }
    std::cout << "\n";
    return g_fail == 0 ? 0 : 1;
}
