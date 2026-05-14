#include "webrtc_sdp.h"

#include <iostream>
#include <string>

using namespace yuan::net::webrtc;

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

bool test_webrtc_sdp_parse_minimal_offer()
{
    const std::string sdp =
        "v=0\r\n"
        "o=- 1 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111 0\r\n"
        "a=mid:0\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=rtpmap:0 PCMU/8000\r\n";

    SdpSession parsed;
    TEST_ASSERT(WebrtcSdp::parse(sdp, parsed), "SDP parse should succeed");
    TEST_ASSERT(parsed.media_sections.size() == 1, "one media section expected");
    TEST_ASSERT(parsed.bundle_group == "0", "bundle group should parse");

    const auto &audio = parsed.media_sections[0];
    TEST_ASSERT(audio.kind == "audio", "media kind should be audio");
    TEST_ASSERT(audio.protocol == "UDP/TLS/RTP/SAVPF", "protocol should parse");
    TEST_ASSERT(audio.mid == "0", "mid should parse");
    TEST_ASSERT(audio.payload_types.size() == 2, "two payload types expected");
    TEST_ASSERT(audio.rtp_maps.size() == 2, "two rtpmap lines expected");
    TEST_ASSERT(audio.rtp_maps[0].payload_type == 111, "first payload should be 111");
    TEST_ASSERT(audio.rtp_maps[0].codec == "opus", "first codec should be opus");
    TEST_ASSERT(audio.rtp_maps[0].clock_rate == 48000, "first clock rate should match");
    TEST_ASSERT(audio.rtp_maps[0].channels == 2, "opus channels should parse");
    return true;
}

bool test_webrtc_sdp_serialize_roundtrip()
{
    SdpSession session;
    session.origin = "- 3 4 IN IP4 127.0.0.1";
    session.session_name = "-";
    session.timing = "0 0";
    session.bundle_group = "0 1";
    session.has_fingerprint = true;
    session.fingerprint.algorithm = "sha-256";
    session.fingerprint.value = "AA:BB:CC";

    SdpMediaSection audio;
    audio.kind = "audio";
    audio.port = 9;
    audio.protocol = "UDP/TLS/RTP/SAVPF";
    audio.mid = "0";
    audio.direction = SdpMediaDirection::sendrecv;
    audio.payload_types = {111};
    audio.rtp_maps.push_back(SdpRtpMap{111, "opus", 48000, 2});

    SdpMediaSection video;
    video.kind = "video";
    video.port = 9;
    video.protocol = "UDP/TLS/RTP/SAVPF";
    video.mid = "1";
    video.direction = SdpMediaDirection::recvonly;
    video.payload_types = {96};
    video.rtp_maps.push_back(SdpRtpMap{96, "VP8", 90000, 1});

    session.media_sections.push_back(audio);
    session.media_sections.push_back(video);

    std::string encoded;
    TEST_ASSERT(WebrtcSdp::serialize(session, encoded), "SDP serialize should succeed");

    SdpSession decoded;
    TEST_ASSERT(WebrtcSdp::parse(encoded, decoded), "serialized SDP should parse again");
    TEST_ASSERT(decoded.media_sections.size() == 2, "two media sections should remain after roundtrip");
    TEST_ASSERT(decoded.media_sections[1].kind == "video", "second media should remain video");
    TEST_ASSERT(decoded.media_sections[1].direction == SdpMediaDirection::recvonly, "video direction should remain recvonly");
    TEST_ASSERT(decoded.media_sections[1].rtp_maps[0].codec == "VP8", "video codec should remain VP8");
    TEST_ASSERT(decoded.has_fingerprint, "fingerprint should roundtrip");
    TEST_ASSERT(decoded.fingerprint.algorithm == "sha-256", "fingerprint algorithm should roundtrip");
    TEST_ASSERT(decoded.fingerprint.value == "AA:BB:CC", "fingerprint value should roundtrip");
    return true;
}

bool test_webrtc_sdp_parse_fingerprint()
{
    const std::string sdp =
        "v=0\r\n"
        "o=- 1 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=fingerprint:sha-256 11:22:33\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=mid:0\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:111 opus/48000/2\r\n";

    SdpSession parsed;
    TEST_ASSERT(WebrtcSdp::parse(sdp, parsed), "SDP with fingerprint should parse");
    TEST_ASSERT(parsed.has_fingerprint, "fingerprint should be present");
    TEST_ASSERT(parsed.fingerprint.algorithm == "sha-256", "fingerprint algorithm should parse");
    TEST_ASSERT(parsed.fingerprint.value == "11:22:33", "fingerprint value should parse");
    return true;
}

bool test_webrtc_sdp_reject_invalid_inputs()
{
    SdpSession parsed;
    TEST_ASSERT(!WebrtcSdp::parse("", parsed), "empty SDP should fail parse");

    const std::string bad_version =
        "v=1\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n";
    TEST_ASSERT(!WebrtcSdp::parse(bad_version, parsed), "non-v=0 SDP should fail parse");

    const std::string bad_rtpmap =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=rtpmap:111\r\n";
    TEST_ASSERT(!WebrtcSdp::parse(bad_rtpmap, parsed), "invalid rtpmap should fail parse");

    SdpSession invalid;
    invalid.media_sections.push_back(SdpMediaSection{});
    std::string out;
    TEST_ASSERT(!WebrtcSdp::serialize(invalid, out), "serialize should fail for invalid empty media section");
    return true;
}

} // namespace

int main()
{
    std::cout << "=== WebRTC SDP Test Suite ===\n";
    RUN_TEST(test_webrtc_sdp_parse_minimal_offer);
    RUN_TEST(test_webrtc_sdp_serialize_roundtrip);
    RUN_TEST(test_webrtc_sdp_parse_fingerprint);
    RUN_TEST(test_webrtc_sdp_reject_invalid_inputs);

    std::cout << "Result: " << g_pass << "/" << g_run << " passed";
    if (g_fail > 0) {
        std::cout << ", " << g_fail << " failed";
    }
    std::cout << "\n";
    return g_fail == 0 ? 0 : 1;
}
