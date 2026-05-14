#include "rtsp_session.h"

#include <iostream>

using namespace yuan::net::rtsp;

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

bool test_rtsp_session_cseq_and_state_machine()
{
    RtspSession session("s1");
    RtspTransportSpec transport;
    transport.transport = RtspLowerTransport::rtp_avp_tcp;
    transport.interleaved_rtp_channel = 0;
    transport.interleaved_rtcp_channel = 1;

    TEST_ASSERT(session.on_setup("rtsp://example/live/track1", transport), "setup should succeed");
    TEST_ASSERT(session.validate_cseq(10), "first cseq should pass");
    TEST_ASSERT(!session.validate_cseq(10), "duplicate cseq should fail");
    TEST_ASSERT(!session.validate_cseq(9), "decreasing cseq should fail");
    TEST_ASSERT(session.validate_cseq(11), "increasing cseq should pass");
    TEST_ASSERT(session.on_play(), "play should succeed");
    TEST_ASSERT(session.on_pause(), "pause should succeed");
    TEST_ASSERT(!session.on_announce(0, ""), "announce should fail with empty media set");
    TEST_ASSERT(session.on_announce(1, "video:96:H264:90000;"), "announce should succeed when track count matches");
    TEST_ASSERT(session.on_announce(1, "video:96:H264:90000;"), "duplicate announce with same payload should be idempotent");
    TEST_ASSERT(!session.on_announce(2, "video:96:H264:90000;audio:97:AAC:48000;"),
                "announce media count mismatch should fail");
    TEST_ASSERT(session.on_record(), "record should succeed after announce");
    session.on_teardown();
    TEST_ASSERT(session.state() == RtspSessionState::closed, "teardown should close session");
    return true;
}

bool test_rtsp_session_timeout_configuration()
{
    RtspSession session("s2");
    session.set_timeout_ms(15000);
    TEST_ASSERT(session.timeout_ms() == 15000, "timeout should update to 15s");

    session.touch(1000);
    TEST_ASSERT(!session.is_expired(12000, session.timeout_ms()), "session should still be alive");
    TEST_ASSERT(session.is_expired(20050, session.timeout_ms()), "session should expire over timeout");

    session.set_timeout_ms(0);
    TEST_ASSERT(session.timeout_ms() == 60000, "timeout 0 should reset to default");
    return true;
}

bool test_rtsp_session_interleaved_channel_mapping()
{
    RtspSession session("s3");

    RtspTransportSpec t1;
    t1.transport = RtspLowerTransport::rtp_avp_tcp;
    t1.interleaved_rtp_channel = 0;
    t1.interleaved_rtcp_channel = 1;
    TEST_ASSERT(session.on_setup("rtsp://example/live/track1", t1), "setup track1 should succeed");

    RtspTransportSpec t2;
    t2.transport = RtspLowerTransport::rtp_avp_tcp;
    t2.interleaved_rtp_channel = 2;
    t2.interleaved_rtcp_channel = 3;
    TEST_ASSERT(!session.has_interleaved_channel_conflict(t2), "disjoint channels should not conflict");
    TEST_ASSERT(session.on_setup("rtsp://example/live/track2", t2), "setup track2 should succeed");

    bool is_rtcp = false;
    TEST_ASSERT(session.resolve_interleaved_channel(0, is_rtcp) && !is_rtcp, "channel 0 should map to RTP");
    TEST_ASSERT(session.resolve_interleaved_channel(1, is_rtcp) && is_rtcp, "channel 1 should map to RTCP");
    TEST_ASSERT(session.resolve_interleaved_channel(2, is_rtcp) && !is_rtcp, "channel 2 should map to RTP");
    TEST_ASSERT(session.resolve_interleaved_channel(3, is_rtcp) && is_rtcp, "channel 3 should map to RTCP");
    TEST_ASSERT(!session.resolve_interleaved_channel(9, is_rtcp), "unknown channel should fail");

    RtspTransportSpec conflict;
    conflict.transport = RtspLowerTransport::rtp_avp_tcp;
    conflict.interleaved_rtp_channel = 1;
    conflict.interleaved_rtcp_channel = 4;
    TEST_ASSERT(session.has_interleaved_channel_conflict(conflict), "overlapped channel should conflict");

    uint8_t rtp_channel = 0;
    uint8_t rtcp_channel = 0;
    TEST_ASSERT(session.resolve_track_interleaved_channels("rtsp://example/live/track1", rtp_channel, rtcp_channel),
                "track1 interleaved mapping should resolve");
    TEST_ASSERT(rtp_channel == 0 && rtcp_channel == 1, "track1 interleaved channels should match setup");
    TEST_ASSERT(session.resolve_track_interleaved_channels("rtsp://example/live/track2", rtp_channel, rtcp_channel),
                "track2 interleaved mapping should resolve");
    TEST_ASSERT(rtp_channel == 2 && rtcp_channel == 3, "track2 interleaved channels should match setup");
    TEST_ASSERT(!session.resolve_track_interleaved_channels("rtsp://example/live/missing", rtp_channel, rtcp_channel),
                "missing track interleaved mapping should fail");
    return true;
}

bool test_rtsp_session_track_mapping_updates_and_udp_rejects_interleaved_lookup()
{
    RtspSession session("s4");

    RtspTransportSpec tcp;
    tcp.transport = RtspLowerTransport::rtp_avp_tcp;
    tcp.interleaved_rtp_channel = 6;
    tcp.interleaved_rtcp_channel = 7;
    TEST_ASSERT(session.on_setup("rtsp://example/live/track1", tcp), "tcp setup should succeed");

    uint8_t rtp_channel = 0;
    uint8_t rtcp_channel = 0;
    TEST_ASSERT(session.resolve_track_interleaved_channels("rtsp://example/live/track1", rtp_channel, rtcp_channel),
                "tcp track mapping should resolve");
    TEST_ASSERT(rtp_channel == 6 && rtcp_channel == 7, "initial channels should match");

    RtspTransportSpec tcp_update;
    tcp_update.transport = RtspLowerTransport::rtp_avp_tcp;
    tcp_update.interleaved_rtp_channel = 10;
    tcp_update.interleaved_rtcp_channel = 11;
    TEST_ASSERT(session.on_setup("rtsp://example/live/track1", tcp_update), "re-setup same track should update mapping");
    TEST_ASSERT(session.resolve_track_interleaved_channels("rtsp://example/live/track1", rtp_channel, rtcp_channel),
                "updated track mapping should resolve");
    TEST_ASSERT(rtp_channel == 10 && rtcp_channel == 11, "updated channels should be reflected");

    RtspTransportSpec udp;
    udp.transport = RtspLowerTransport::rtp_avp_udp;
    udp.client_rtp_port = 4000;
    udp.client_rtcp_port = 4001;
    TEST_ASSERT(session.on_setup("rtsp://example/live/track2", udp), "udp setup should succeed");
    TEST_ASSERT(!session.resolve_track_interleaved_channels("rtsp://example/live/track2", rtp_channel, rtcp_channel),
                "udp track should not expose interleaved channel mapping");
    return true;
}

} // namespace

int main()
{
    std::cout << "=== RTSP Session Test Suite ===\n";
    RUN_TEST(test_rtsp_session_cseq_and_state_machine);
    RUN_TEST(test_rtsp_session_timeout_configuration);
    RUN_TEST(test_rtsp_session_interleaved_channel_mapping);
    RUN_TEST(test_rtsp_session_track_mapping_updates_and_udp_rejects_interleaved_lookup);

    std::cout << "Result: " << g_pass << "/" << g_run << " passed";
    if (g_fail > 0) {
        std::cout << ", " << g_fail << " failed";
    }
    std::cout << "\n";
    return g_fail == 0 ? 0 : 1;
}
