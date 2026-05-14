#include "rtsp.h"

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
        std::cout << "Running " #func "...\n";                                               \
        if (func()) {                                                                            \
            ++g_pass;                                                                            \
            std::cout << "  PASS\n";                                                           \
        } else {                                                                                 \
            ++g_fail;                                                                            \
            std::cout << "  FAIL\n";                                                           \
        }                                                                                        \
    } while (0)

bool test_rtsp_method_and_status_helpers()
{
    TEST_ASSERT(parse_method("OPTIONS") == RtspMethod::options, "OPTIONS should parse");
    TEST_ASSERT(parse_method("PLAY") == RtspMethod::play, "PLAY should parse");
    TEST_ASSERT(parse_method("UNKNOWN") == RtspMethod::unknown, "unknown should fallback");
    TEST_ASSERT(std::string(method_to_string(RtspMethod::teardown)) == "TEARDOWN", "method text should match");
    TEST_ASSERT(std::string(status_code_reason(RtspStatusCode::ok)) == "OK", "status reason should match");
    TEST_ASSERT(std::string(status_code_reason(RtspStatusCode::request_timeout)) == "Request Timeout", "408 reason should match");
    return true;
}

bool test_rtsp_parser_request_and_response_roundtrip()
{
    const std::string raw =
        "DESCRIBE rtsp://example.com/live RTSP/1.0\r\n"
        "CSeq: 2\r\n"
        "Accept: application/sdp\r\n"
        "\r\n";

    RtspRequest request;
    TEST_ASSERT(RtspParser::parse_request(raw, request), "request should parse");
    TEST_ASSERT(request.method == RtspMethod::describe, "method should be DESCRIBE");
    TEST_ASSERT(request.cseq == 2, "cseq should parse");
    TEST_ASSERT(request.header("accept") != nullptr, "header lookup should be case-insensitive");

    RtspResponse response;
    response.status = RtspStatusCode::ok;
    response.cseq = request.cseq;
    response.headers["Content-Type"] = "application/sdp";
    response.body = "v=0\r\n";

    const std::string encoded = RtspParser::serialize_response(response);
    TEST_ASSERT(encoded.find("RTSP/1.0 200 OK") == 0, "status line should match");
    TEST_ASSERT(encoded.find("CSeq: 2") != std::string::npos, "response cseq should exist");
    TEST_ASSERT(encoded.find("Content-Length: 5") != std::string::npos, "content-length should be auto generated");
    return true;
}

bool test_rtsp_session_transport_and_sdp_basics()
{
    RtspSession session("sid-001");
    TEST_ASSERT(session.state() == RtspSessionState::idle, "initial state should be idle");
    RtspTransportSpec session_transport;
    session_transport.transport = RtspLowerTransport::rtp_avp_tcp;
    session_transport.interleaved_rtp_channel = 0;
    session_transport.interleaved_rtcp_channel = 1;

    TEST_ASSERT(session.on_setup("rtsp://example/live/track1", session_transport), "setup should succeed");
    TEST_ASSERT(session.has_transport(), "transport should be stored after setup");
    TEST_ASSERT(session.transport().interleaved_rtp_channel == 0, "stored transport should match");
    TEST_ASSERT(session.validate_cseq(1), "first cseq should be accepted");
    TEST_ASSERT(!session.validate_cseq(1), "duplicate cseq should be rejected");
    TEST_ASSERT(session.validate_cseq(2), "increasing cseq should be accepted");
    TEST_ASSERT(session.on_play(), "play should succeed from ready");
    TEST_ASSERT(session.on_pause(), "pause should return to ready");
    TEST_ASSERT(!session.on_record(), "record should fail before announce");
    session.set_announced(true);
    TEST_ASSERT(session.on_record(), "record should succeed after announce");
    session.on_teardown();
    TEST_ASSERT(session.state() == RtspSessionState::closed, "teardown should close session");

    RtspTransportSpec spec;
    TEST_ASSERT(parse_transport_header("RTP/AVP/TCP;unicast;interleaved=0-1", spec), "tcp interleaved transport should parse");
    TEST_ASSERT(spec.transport == RtspLowerTransport::rtp_avp_tcp, "transport type should be tcp");
    TEST_ASSERT(spec.interleaved_rtp_channel == 0 && spec.interleaved_rtcp_channel == 1, "interleaved channels should parse");

    RtspSdpDescription desc;
    desc.session_name = "demo";
    desc.media.push_back({"video", 96, "H264", 90000});

    std::string sdp;
    TEST_ASSERT(serialize_sdp(desc, sdp), "sdp serialize should succeed");
    RtspSdpDescription parsed;
    TEST_ASSERT(parse_sdp(sdp, parsed), "sdp parse should succeed");
    TEST_ASSERT(!parsed.media.empty(), "parsed sdp should contain media");
    TEST_ASSERT(parsed.media[0].payload_type == 96, "payload type should remain 96");
    return true;
}

bool test_rtsp_server_default_control_flow()
{
    RtspServer server;
    RtspResponse response;

    server.set_handler([&response](const RtspRequest &, RtspResponse &out) {
        out = response;
    });

    const std::string setup_raw =
        "SETUP rtsp://example.com/live/track1 RTSP/1.0\r\n"
        "CSeq: 3\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
        "\r\n";
    RtspRequest setup_req;
    TEST_ASSERT(RtspParser::parse_request(setup_raw, setup_req), "setup request should parse");

    response = {};
    response.cseq = setup_req.cseq;
    response.status = RtspStatusCode::ok;
    response.headers["Session"] = "sid-test";
    response.headers["Transport"] = "RTP/AVP/TCP;unicast;interleaved=0-1";
    std::string setup_resp_text = RtspParser::serialize_response(response);
    TEST_ASSERT(setup_resp_text.find("Session: sid-test") != std::string::npos, "setup response should contain session");

    const std::string play_raw =
        "PLAY rtsp://example.com/live RTSP/1.0\r\n"
        "CSeq: 4\r\n"
        "Session: sid-test\r\n"
        "\r\n";
    RtspRequest play_req;
    TEST_ASSERT(RtspParser::parse_request(play_raw, play_req), "play request should parse");
    response = {};
    response.cseq = play_req.cseq;
    response.status = RtspStatusCode::ok;
    response.headers["Session"] = "sid-test";
    response.headers["RTP-Info"] = "url=rtsp://example.com/live;seq=0;rtptime=0";
    const std::string play_resp_text = RtspParser::serialize_response(response);
    TEST_ASSERT(play_resp_text.find("RTP-Info:") != std::string::npos, "play response should contain rtp-info");
    return true;
}

bool test_rtsp_session_expire_logic()
{
    RtspSession session("sid-expire");
    RtspTransportSpec transport;
    transport.transport = RtspLowerTransport::rtp_avp_udp;
    transport.client_rtp_port = 4000;
    transport.client_rtcp_port = 4001;
    TEST_ASSERT(session.on_setup("rtsp://example/live/track1", transport), "setup should succeed");

    session.touch(1000);
    TEST_ASSERT(!session.is_expired(1500, 1000), "session should not expire within timeout");
    TEST_ASSERT(session.is_expired(2501, 1000), "session should expire beyond timeout");
    TEST_ASSERT(!session.is_expired(2501, 0), "timeout=0 should disable expiration");
    return true;
}

bool test_rtsp_status_reason_additional_codes()
{
    TEST_ASSERT(std::string(status_code_reason(RtspStatusCode::session_not_found)) == "Session Not Found", "454 reason should match");
    TEST_ASSERT(
        std::string(status_code_reason(RtspStatusCode::method_not_valid_in_this_state)) == "Method Not Valid In This State",
        "455 reason should match");
    return true;
}

bool test_rtsp_transport_multi_candidate_parse()
{
    std::vector<RtspTransportSpec> specs;
    TEST_ASSERT(parse_transport_candidates(
                    "RTP/AVP/TCP;multicast;interleaved=0-1, RTP/AVP;unicast;client_port=4000-4001",
                    specs),
                "transport candidate list should parse");
    TEST_ASSERT(specs.size() == 2, "two transport candidates expected");
    TEST_ASSERT(specs[0].transport == RtspLowerTransport::rtp_avp_tcp, "first candidate should be tcp");
    TEST_ASSERT(specs[1].transport == RtspLowerTransport::rtp_avp_udp, "second candidate should be udp");
    return true;
}

bool test_rtsp_framer_parser_smoke()
{
    RtspStreamFramer framer;
    RtspFrame frame;

    framer.push("OPTIONS rtsp://example/live RTSP/1.0\r\nCSeq: 42\r\n\r\n");
    TEST_ASSERT(framer.pop(frame) == RtspFrameParseResult::ok, "framer should parse rtsp request");
    TEST_ASSERT(frame.kind == RtspFrameKind::rtsp_request, "frame should be rtsp kind");

    RtspRequest req;
    TEST_ASSERT(RtspParser::parse_request(frame.data, req), "parsed frame should decode as request");
    TEST_ASSERT(req.method == RtspMethod::options, "decoded method should be options");

    std::string inter;
    inter.push_back('$');
    inter.push_back(static_cast<char>(9));
    inter.push_back(0);
    inter.push_back(3);
    inter += "xyz";
    framer.push(inter);

    TEST_ASSERT(framer.pop(frame) == RtspFrameParseResult::ok, "framer should parse interleaved");
    TEST_ASSERT(frame.kind == RtspFrameKind::interleaved, "frame should be interleaved kind");
    TEST_ASSERT(frame.channel == 9, "channel should match");
    TEST_ASSERT(frame.data == "xyz", "payload should match");
    return true;
}

} // namespace

int main()
{
    std::cout << "=== RTSP Protocol Test Suite ===\n";
    RUN_TEST(test_rtsp_method_and_status_helpers);
    RUN_TEST(test_rtsp_parser_request_and_response_roundtrip);
    RUN_TEST(test_rtsp_session_transport_and_sdp_basics);
    RUN_TEST(test_rtsp_server_default_control_flow);
    RUN_TEST(test_rtsp_session_expire_logic);
    RUN_TEST(test_rtsp_status_reason_additional_codes);
    RUN_TEST(test_rtsp_transport_multi_candidate_parse);
    RUN_TEST(test_rtsp_framer_parser_smoke);

    std::cout << "Result: " << g_pass << "/" << g_run << " passed";
    if (g_fail > 0) {
        std::cout << ", " << g_fail << " failed";
    }
    std::cout << "\n";
    return g_fail == 0 ? 0 : 1;
}
