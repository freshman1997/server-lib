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
    p.payload = {0x01, 0x02};
    return p;
}

bool test_manager_multi_stream_dispatch()
{
    RtpSessionManager manager(90000);

    TEST_ASSERT(manager.on_packet_received(make_packet(0x10101010, 10, 0), 0), "ssrc A packet1 accepted");
    TEST_ASSERT(manager.on_packet_received(make_packet(0x20202020, 20, 0), 0), "ssrc B packet1 accepted");
    TEST_ASSERT(manager.on_packet_received(make_packet(0x10101010, 11, 3000), 33), "ssrc A packet2 accepted");
    TEST_ASSERT(manager.on_packet_received(make_packet(0x20202020, 21, 3000), 33), "ssrc B packet2 accepted");

    TEST_ASSERT(manager.session_count() == 2, "two sessions should exist");
    auto *a = manager.find_session(0x10101010);
    auto *b = manager.find_session(0x20202020);
    TEST_ASSERT(a != nullptr, "session A should exist");
    TEST_ASSERT(b != nullptr, "session B should exist");
    TEST_ASSERT(a->receive_stats().packets_received == 2, "session A receive count should be 2");
    TEST_ASSERT(b->receive_stats().packets_received == 2, "session B receive count should be 2");
    return true;
}

bool test_manager_conflict_reject_policy()
{
    RtpSessionManager manager(90000);
    manager.set_conflict_policy(RtpSsrcConflictPolicy::reject);

    auto conflicting = std::make_unique<RtpSession>(0xAAAAAAAA, 90000);
    TEST_ASSERT(manager.attach_session(0x30303030, std::move(conflicting)), "conflicting session should be attached");

    TEST_ASSERT(!manager.on_packet_received(make_packet(0x30303030, 101, 3000), 33), "conflict should be rejected");
    TEST_ASSERT(manager.rejected_packets() == 1, "rejected counter should be 1");

    auto *session = manager.find_session(0x30303030);
    TEST_ASSERT(session != nullptr, "session should still exist");
    TEST_ASSERT(session->ssrc() == 0xAAAAAAAA, "session should keep old owner under reject policy");
    return true;
}

bool test_manager_conflict_replace_policy()
{
    RtpSessionManager manager(90000);
    manager.set_conflict_policy(RtpSsrcConflictPolicy::replace);

    auto conflicting = std::make_unique<RtpSession>(0xBBBBBBBB, 90000);
    TEST_ASSERT(manager.attach_session(0x40404040, std::move(conflicting)), "conflicting session should be attached");

    TEST_ASSERT(manager.on_packet_received(make_packet(0x40404040, 1, 6000), 66), "replace policy should recover from conflict");

    auto *after = manager.find_session(0x40404040);
    TEST_ASSERT(after != nullptr, "session should still exist");
    TEST_ASSERT(after->ssrc() == 0x40404040, "session should be replaced by correct ssrc owner");
    TEST_ASSERT(after->receive_stats().packets_received == 1, "replaced session should start fresh");
    return true;
}

} // namespace

int main()
{
    std::cout << "=== RTP Session Manager Test Suite ===\n";
    RUN_TEST(test_manager_multi_stream_dispatch);
    RUN_TEST(test_manager_conflict_reject_policy);
    RUN_TEST(test_manager_conflict_replace_policy);

    std::cout << "Result: " << g_pass << "/" << g_run << " passed";
    if (g_fail > 0) {
        std::cout << ", " << g_fail << " failed";
    }
    std::cout << "\n";
    return g_fail == 0 ? 0 : 1;
}
