#include "rtsp_parser.h"
#include "rtsp_server.h"

#include "base/utils/base64.h"
#include "buffer/byte_buffer.h"
#include "rtc_packet.h"

#include <iostream>
#include <string>

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
        std::cout << "Running " #func "...\n";                                                \
        if (func()) {                                                                            \
            ++g_pass;                                                                            \
            std::cout << "  PASS\n";                                                           \
        } else {                                                                                 \
            ++g_fail;                                                                            \
            std::cout << "  FAIL\n";                                                           \
        }                                                                                        \
    } while (0)

bool parse_request_or_fail(const std::string &raw, RtspRequest &out)
{
    if (!RtspParser::parse_request(raw, out)) {
        std::cout << "  FAIL: request fixture parse failed\n";
        return false;
    }
    return true;
}

std::string session_id_from(const RtspResponse &resp)
{
    auto it = resp.headers.find("Session");
    if (it == resp.headers.end()) {
        return {};
    }
    const auto sep = it->second.find(';');
    if (sep == std::string::npos) {
        return it->second;
    }
    return it->second.substr(0, sep);
}

bool test_ffmpeg_tcp_pull_sequence()
{
    RtspServer server;

    RtspRequest setup;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 1\r\n"
            "User-Agent: Lavf/61.1.100\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1, RTP/AVP;unicast;client_port=5000-5001\r\n"
            "\r\n",
            setup)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "ffmpeg setup should succeed");
    TEST_ASSERT(setup_resp.headers.find("Transport") != setup_resp.headers.end(), "transport header should exist");
    TEST_ASSERT(setup_resp.headers.at("Transport").find("interleaved=0-1") != std::string::npos,
                "ffmpeg should prefer first tcp candidate");

    const std::string sid = session_id_from(setup_resp);
    TEST_ASSERT(!sid.empty(), "setup should provide session");

    RtspRequest play;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 2\r\n"
            "Session: " + sid + "\r\n"
            "Range: npt=0-\r\n"
            "\r\n",
            play)) {
        return false;
    }
    const RtspResponse play_resp = server.handle_request(play);
    TEST_ASSERT(play_resp.status == RtspStatusCode::ok, "ffmpeg play should succeed");
    TEST_ASSERT(play_resp.headers.find("RTP-Info") != play_resp.headers.end(), "play should include rtp-info");
    return true;
}

bool test_vlc_udp_pull_sequence()
{
    RtspServer server;

    RtspRequest setup;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/vlc/track1 RTSP/1.0\r\n"
            "CSeq: 10\r\n"
            "User-Agent: LibVLC/3.0.20\r\n"
            "Transport: RTP/AVP;unicast;client_port=1234-1235\r\n"
            "\r\n",
            setup)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "vlc udp setup should succeed");
    TEST_ASSERT(setup_resp.headers.find("Transport") != setup_resp.headers.end(), "transport header should exist");
    TEST_ASSERT(setup_resp.headers.at("Transport").find("server_port=") != std::string::npos,
                "udp setup should allocate server ports");

    const std::string sid = session_id_from(setup_resp);
    TEST_ASSERT(!sid.empty(), "udp setup should provide session");

    RtspRequest play;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/vlc RTSP/1.0\r\n"
            "CSeq: 11\r\n"
            "Session: " + sid + "\r\n"
            "Range: npt=now-\r\n"
            "\r\n",
            play)) {
        return false;
    }
    const RtspResponse play_resp = server.handle_request(play);
    TEST_ASSERT(play_resp.status == RtspStatusCode::ok, "vlc play should succeed");
    TEST_ASSERT(play_resp.headers.find("Range") != play_resp.headers.end(), "range should be present");
    TEST_ASSERT(play_resp.headers.at("Range") == "npt=now-", "now-range should keep canonical form");
    return true;
}

bool test_gstreamer_push_dual_track_sequence()
{
    RtspServer server;

    RtspRequest setup_video;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/push/track1 RTSP/1.0\r\n"
            "CSeq: 20\r\n"
            "User-Agent: GStreamer/1.24\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            setup_video)) {
        return false;
    }
    const RtspResponse setup_video_resp = server.handle_request(setup_video);
    TEST_ASSERT(setup_video_resp.status == RtspStatusCode::ok, "gstreamer video setup should succeed");
    const std::string sid = session_id_from(setup_video_resp);
    TEST_ASSERT(!sid.empty(), "gstreamer video setup should return session");

    RtspRequest setup_audio;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/push/track2 RTSP/1.0\r\n"
            "CSeq: 21\r\n"
            "Session: " + sid + "\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n"
            "\r\n",
            setup_audio)) {
        return false;
    }
    const RtspResponse setup_audio_resp = server.handle_request(setup_audio);
    TEST_ASSERT(setup_audio_resp.status == RtspStatusCode::ok, "gstreamer audio setup should succeed");

    RtspRequest announce;
    if (!parse_request_or_fail(
            "ANNOUNCE rtsp://example/push RTSP/1.0\r\n"
            "CSeq: 22\r\n"
            "Session: " + sid + "\r\n"
            "\r\n"
            "v=0\r\n"
            "o=- 1 1 IN IP4 127.0.0.1\r\n"
            "s=push\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 H264/90000\r\n"
            "m=audio 0 RTP/AVP 97\r\n"
            "a=rtpmap:97 AAC/48000\r\n",
            announce)) {
        return false;
    }
    const RtspResponse announce_resp = server.handle_request(announce);
    TEST_ASSERT(announce_resp.status == RtspStatusCode::ok, "gstreamer announce should succeed");

    RtspRequest record;
    if (!parse_request_or_fail(
            "RECORD rtsp://example/push RTSP/1.0\r\n"
            "CSeq: 23\r\n"
            "Session: " + sid + "\r\n"
            "\r\n",
            record)) {
        return false;
    }
    const RtspResponse record_resp = server.handle_request(record);
    TEST_ASSERT(record_resp.status == RtspStatusCode::ok, "gstreamer record should succeed");

    ::yuan::net::rtc::RtcPacket media;
    media.ssrc = 0x11223344u;
    media.sequence_number = 7;
    media.timestamp = 3333;
    media.payload_type = 96;
    media.payload = {0x10, 0x20};

    RtspInterleavedFrame out;
    TEST_ASSERT(server.build_interleaved_rtp_frame(sid, "rtsp://example/push/track1", media, out),
                "track1 rtp frame should build");
    TEST_ASSERT(out.channel == 0, "track1 rtp should map to channel 0");
    TEST_ASSERT(server.build_interleaved_receiver_report_frame(sid, "rtsp://example/push/track2", out),
                "track2 rr frame should build");
    TEST_ASSERT(out.channel == 3, "track2 rtcp should map to channel 3");
    return true;
}

bool test_mixed_auth_challenge_for_common_clients()
{
    RtspServer server;
    server.configure_basic_auth("cam", "alice", "secret");
    server.configure_digest_auth("cam", "alice", "secret");

    RtspRequest no_auth;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/auth/track1 RTSP/1.0\r\n"
            "CSeq: 30\r\n"
            "User-Agent: LibVLC/3.0.20\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            no_auth)) {
        return false;
    }
    const RtspResponse no_auth_resp = server.handle_request(no_auth);
    TEST_ASSERT(no_auth_resp.status == RtspStatusCode::unauthorized, "no auth should get 401");
    TEST_ASSERT(no_auth_resp.headers.find("WWW-Authenticate") != no_auth_resp.headers.end(), "challenge should exist");

    const std::string challenge = no_auth_resp.headers.at("WWW-Authenticate");
    TEST_ASSERT(challenge.find("Basic realm=\"cam\"") != std::string::npos, "basic challenge should exist");
    TEST_ASSERT(challenge.find("Digest realm=\"cam\"") != std::string::npos, "digest challenge should exist");

    const std::string basic = "Basic " + ::yuan::base::util::base64_encode("alice:secret");
    RtspRequest basic_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/auth/track1 RTSP/1.0\r\n"
            "CSeq: 31\r\n"
            "Authorization: " + basic + "\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            basic_req)) {
        return false;
    }
    const RtspResponse basic_resp = server.handle_request(basic_req);
    TEST_ASSERT(basic_resp.status == RtspStatusCode::ok, "basic fallback should pass");
    return true;
}

} // namespace

int main()
{
    std::cout << "=== RTSP Interop Matrix Test Suite ===\n";
    RUN_TEST(test_ffmpeg_tcp_pull_sequence);
    RUN_TEST(test_vlc_udp_pull_sequence);
    RUN_TEST(test_gstreamer_push_dual_track_sequence);
    RUN_TEST(test_mixed_auth_challenge_for_common_clients);

    std::cout << "Result: " << g_pass << "/" << g_run << " passed";
    if (g_fail > 0) {
        std::cout << ", " << g_fail << " failed";
    }
    std::cout << "\n";
    return g_fail == 0 ? 0 : 1;
}
