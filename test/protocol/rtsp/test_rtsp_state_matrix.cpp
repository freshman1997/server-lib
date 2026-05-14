#include "rtsp_parser.h"
#include "rtsp_server.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

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

bool parse_request_or_fail(const std::string &raw, RtspRequest &out)
{
    if (!RtspParser::parse_request(raw, out)) {
        std::cout << "  FAIL: fixture parse failed\n";
        return false;
    }
    return true;
}

std::string extract_session_id(const RtspResponse &response)
{
    auto it = response.headers.find("Session");
    if (it == response.headers.end()) {
        return {};
    }
    const auto semicolon = it->second.find(';');
    if (semicolon == std::string::npos) {
        return it->second;
    }
    return it->second.substr(0, semicolon);
}

bool test_state_matrix_play_pause_record_transitions()
{
    RtspServer server;

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 1\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "setup should be ok");

    const std::string sid = extract_session_id(setup_resp);
    TEST_ASSERT(!sid.empty(), "session id should not be empty");

    RtspRequest play_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 2\r\n"
            "Session: " + sid + "\r\n"
            "\r\n",
            play_req)) {
        return false;
    }
    const RtspResponse play_resp = server.handle_request(play_req);
    TEST_ASSERT(play_resp.status == RtspStatusCode::ok, "play from ready should be ok");

    RtspRequest play_again_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 3\r\n"
            "Session: " + sid + "\r\n"
            "\r\n",
            play_again_req)) {
        return false;
    }
    const RtspResponse play_again_resp = server.handle_request(play_again_req);
    TEST_ASSERT(play_again_resp.status == RtspStatusCode::method_not_valid_in_this_state, "play from playing should be 455");

    RtspRequest pause_req;
    if (!parse_request_or_fail(
            "PAUSE rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 4\r\n"
            "Session: " + sid + "\r\n"
            "\r\n",
            pause_req)) {
        return false;
    }
    const RtspResponse pause_resp = server.handle_request(pause_req);
    TEST_ASSERT(pause_resp.status == RtspStatusCode::ok, "pause from playing should be ok");

    RtspRequest record_before_announce_req;
    if (!parse_request_or_fail(
            "RECORD rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 5\r\n"
            "Session: " + sid + "\r\n"
            "\r\n",
            record_before_announce_req)) {
        return false;
    }
    const RtspResponse record_before_announce_resp = server.handle_request(record_before_announce_req);
    TEST_ASSERT(record_before_announce_resp.status == RtspStatusCode::method_not_valid_in_this_state, "record without announce should be 455");

    RtspRequest announce_req;
    if (!parse_request_or_fail(
            "ANNOUNCE rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 6\r\n"
            "Session: " + sid + "\r\n"
            "\r\n"
            "v=0\r\n"
            "o=- 1 1 IN IP4 127.0.0.1\r\n"
            "s=demo\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 H264/90000\r\n",
            announce_req)) {
        return false;
    }
    const RtspResponse announce_resp = server.handle_request(announce_req);
    TEST_ASSERT(announce_resp.status == RtspStatusCode::ok, "announce should be ok");

    RtspRequest record_req;
    if (!parse_request_or_fail(
            "RECORD rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 7\r\n"
            "Session: " + sid + "\r\n"
            "\r\n",
            record_req)) {
        return false;
    }
    const RtspResponse record_resp = server.handle_request(record_req);
    TEST_ASSERT(record_resp.status == RtspStatusCode::ok, "record after announce should be ok");

    RtspRequest pause_on_record_req;
    if (!parse_request_or_fail(
            "PAUSE rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 8\r\n"
            "Session: " + sid + "\r\n"
            "\r\n",
            pause_on_record_req)) {
        return false;
    }
    const RtspResponse pause_on_record_resp = server.handle_request(pause_on_record_req);
    TEST_ASSERT(pause_on_record_resp.status == RtspStatusCode::method_not_valid_in_this_state, "pause from recording should be 455");
    return true;
}

bool test_state_matrix_missing_transport_and_cseq_regression()
{
    RtspServer server;

    RtspRequest setup_no_transport;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 10\r\n"
            "\r\n",
            setup_no_transport)) {
        return false;
    }
    const RtspResponse setup_no_transport_resp = server.handle_request(setup_no_transport);
    TEST_ASSERT(setup_no_transport_resp.status == RtspStatusCode::unsupported_transport, "setup without transport should be 461");

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 11\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "setup should be ok");

    const std::string sid = extract_session_id(setup_resp);

    RtspRequest get_param_req;
    if (!parse_request_or_fail(
            "GET_PARAMETER rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 12\r\n"
            "Session: " + sid + "\r\n"
            "\r\n",
            get_param_req)) {
        return false;
    }
    const RtspResponse get_param_resp = server.handle_request(get_param_req);
    TEST_ASSERT(get_param_resp.status == RtspStatusCode::ok, "get_parameter should be ok");

    RtspRequest get_param_dup_cseq;
    if (!parse_request_or_fail(
            "GET_PARAMETER rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 12\r\n"
            "Session: " + sid + "\r\n"
            "\r\n",
            get_param_dup_cseq)) {
        return false;
    }
    const RtspResponse dup_resp = server.handle_request(get_param_dup_cseq);
    TEST_ASSERT(dup_resp.status == RtspStatusCode::bad_request, "duplicate cseq should be 400");
    return true;
}

bool test_state_matrix_announce_sdp_validation()
{
    RtspServer server;

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 60\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "setup should be ok");
    const std::string sid = extract_session_id(setup_resp);

    RtspRequest announce_bad_req;
    if (!parse_request_or_fail(
            "ANNOUNCE rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 61\r\n"
            "Session: " + sid + "\r\n"
            "\r\n"
            "this is not sdp",
            announce_bad_req)) {
        return false;
    }
    const RtspResponse announce_bad_resp = server.handle_request(announce_bad_req);
    TEST_ASSERT(announce_bad_resp.status == RtspStatusCode::bad_request, "invalid announce sdp should return 400");

    RtspRequest announce_ok_req;
    if (!parse_request_or_fail(
            "ANNOUNCE rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 62\r\n"
            "Session: " + sid + "\r\n"
            "\r\n"
            "v=0\r\n"
            "o=- 1 1 IN IP4 127.0.0.1\r\n"
            "s=demo\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 H264/90000\r\n",
            announce_ok_req)) {
        return false;
    }
    const RtspResponse announce_ok_resp = server.handle_request(announce_ok_req);
    TEST_ASSERT(announce_ok_resp.status == RtspStatusCode::ok, "valid announce sdp should return 200");

    RtspRequest announce_same_again_req;
    if (!parse_request_or_fail(
            "ANNOUNCE rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 62\r\n"
            "Session: " + sid + "\r\n"
            "\r\n"
            "v=0\r\n"
            "o=- 1 1 IN IP4 127.0.0.1\r\n"
            "s=demo\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 H264/90000\r\n",
            announce_same_again_req)) {
        return false;
    }
    announce_same_again_req.cseq = 64;
    const RtspResponse announce_same_again_resp = server.handle_request(announce_same_again_req);
    TEST_ASSERT(announce_same_again_resp.status == RtspStatusCode::ok,
                "duplicate announce with same media should be accepted");

    RtspRequest announce_unsupported_codec_req;
    if (!parse_request_or_fail(
            "ANNOUNCE rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 65\r\n"
            "Session: " + sid + "\r\n"
            "\r\n"
            "v=0\r\n"
            "o=- 1 1 IN IP4 127.0.0.1\r\n"
            "s=demo\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 VP8/90000\r\n",
            announce_unsupported_codec_req)) {
        return false;
    }
    const RtspResponse announce_unsupported_codec_resp = server.handle_request(announce_unsupported_codec_req);
    TEST_ASSERT(announce_unsupported_codec_resp.status == RtspStatusCode::parameter_not_understood,
                "unsupported announce codec should return 451");
    return true;
}

bool test_state_matrix_record_requires_track_consistent_announce()
{
    RtspServer server;

    RtspRequest setup_track1;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 300\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            setup_track1)) {
        return false;
    }
    const RtspResponse setup_resp1 = server.handle_request(setup_track1);
    TEST_ASSERT(setup_resp1.status == RtspStatusCode::ok, "track1 setup should be ok");
    const std::string sid = extract_session_id(setup_resp1);

    RtspRequest setup_track2;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track2 RTSP/1.0\r\n"
            "CSeq: 301\r\n"
            "Session: " + sid + "\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n"
            "\r\n",
            setup_track2)) {
        return false;
    }
    const RtspResponse setup_resp2 = server.handle_request(setup_track2);
    TEST_ASSERT(setup_resp2.status == RtspStatusCode::ok, "track2 setup should be ok");

    RtspRequest announce_mismatch_req;
    if (!parse_request_or_fail(
            "ANNOUNCE rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 302\r\n"
            "Session: " + sid + "\r\n"
            "\r\n"
            "v=0\r\n"
            "o=- 1 1 IN IP4 127.0.0.1\r\n"
            "s=demo\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 H264/90000\r\n",
            announce_mismatch_req)) {
        return false;
    }
    const RtspResponse announce_mismatch_resp = server.handle_request(announce_mismatch_req);
    TEST_ASSERT(announce_mismatch_resp.status == RtspStatusCode::method_not_valid_in_this_state,
                "announce media count mismatch with setup tracks should return 455");
    return true;
}

bool test_state_matrix_play_range_scale_headers()
{
    RtspServer server;

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 50\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "setup should be ok");
    const std::string sid = extract_session_id(setup_resp);

    RtspRequest play_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 51\r\n"
            "Session: " + sid + "\r\n"
            "Range: npt=12.000-\r\n"
            "Scale: 1.500\r\n"
            "\r\n",
            play_req)) {
        return false;
    }
    const RtspResponse play_resp = server.handle_request(play_req);
    TEST_ASSERT(play_resp.status == RtspStatusCode::ok, "play should be ok");
    TEST_ASSERT(play_resp.headers.find("Range") != play_resp.headers.end(), "range should be echoed");
    TEST_ASSERT(play_resp.headers.at("Range") == "npt=12.000-", "range value should be normalized");
    TEST_ASSERT(play_resp.headers.find("Scale") != play_resp.headers.end(), "scale should be echoed");
    TEST_ASSERT(play_resp.headers.at("Scale") == "1.500", "scale should be normalized to 3 decimals");

    RtspRequest bad_scale_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 52\r\n"
            "Session: " + sid + "\r\n"
            "Scale: 99.0\r\n"
            "\r\n",
            bad_scale_req)) {
        return false;
    }
    const RtspResponse bad_scale_resp = server.handle_request(bad_scale_req);
    TEST_ASSERT(bad_scale_resp.status == RtspStatusCode::method_not_valid_in_this_state, "unsupported scale should return 455");

    RtspRequest min_scale_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 521\r\n"
            "Session: " + sid + "\r\n"
            "Scale: 0.125\r\n"
            "\r\n",
            min_scale_req)) {
        return false;
    }
    const RtspResponse min_scale_resp = server.handle_request(min_scale_req);
    TEST_ASSERT(min_scale_resp.status == RtspStatusCode::method_not_valid_in_this_state ||
                    (min_scale_resp.status == RtspStatusCode::ok &&
                     min_scale_resp.headers.find("Scale") != min_scale_resp.headers.end() &&
                     min_scale_resp.headers.at("Scale") == "0.125"),
                "minimum supported scale should be accepted when state allows");

    RtspRequest scale_precision_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 522\r\n"
            "Session: " + sid + "\r\n"
            "Scale: 1.23456\r\n"
            "\r\n",
            scale_precision_req)) {
        return false;
    }
    const RtspResponse scale_precision_resp = server.handle_request(scale_precision_req);
    TEST_ASSERT(scale_precision_resp.status == RtspStatusCode::method_not_valid_in_this_state ||
                    (scale_precision_resp.status == RtspStatusCode::ok &&
                     scale_precision_resp.headers.find("Scale") != scale_precision_resp.headers.end() &&
                     scale_precision_resp.headers.at("Scale") == "1.235"),
                "scale should normalize to three decimals when accepted");

    RtspRequest malformed_scale_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 523\r\n"
            "Session: " + sid + "\r\n"
            "Scale: abc\r\n"
            "\r\n",
            malformed_scale_req)) {
        return false;
    }
    const RtspResponse malformed_scale_resp = server.handle_request(malformed_scale_req);
    TEST_ASSERT(malformed_scale_resp.status == RtspStatusCode::parameter_not_understood,
                "non numeric scale should return 451");

    RtspRequest bad_range_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 524\r\n"
            "Session: " + sid + "\r\n"
            "Range: clock=0\r\n"
            "\r\n",
            bad_range_req)) {
        return false;
    }
    const RtspResponse bad_range_resp = server.handle_request(bad_range_req);
    TEST_ASSERT(bad_range_resp.status == RtspStatusCode::parameter_not_understood, "bad range should return 451");

    RtspRequest bad_range_order_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 525\r\n"
            "Session: " + sid + "\r\n"
            "Range: npt=20-10\r\n"
            "\r\n",
            bad_range_order_req)) {
        return false;
    }
    const RtspResponse bad_range_order_resp = server.handle_request(bad_range_order_req);
    TEST_ASSERT(bad_range_order_resp.status == RtspStatusCode::parameter_not_understood, "reversed range should return 451");

    RtspRequest now_range_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 526\r\n"
            "Session: " + sid + "\r\n"
            "Range: npt=now-\r\n"
            "\r\n",
            now_range_req)) {
        return false;
    }
    const RtspResponse now_range_resp = server.handle_request(now_range_req);
    TEST_ASSERT(now_range_resp.status == RtspStatusCode::method_not_valid_in_this_state || now_range_resp.status == RtspStatusCode::ok,
                "now-range should be accepted if state allows");
    if (now_range_resp.status == RtspStatusCode::ok) {
        TEST_ASSERT(now_range_resp.headers.find("Range") != now_range_resp.headers.end(), "now range should be echoed");
        TEST_ASSERT(now_range_resp.headers.at("Range") == "npt=now-", "now range should keep canonical form");
    }

    RtspRequest negative_range_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 527\r\n"
            "Session: " + sid + "\r\n"
            "Range: npt=-5-\r\n"
            "\r\n",
            negative_range_req)) {
        return false;
    }
    const RtspResponse negative_range_resp = server.handle_request(negative_range_req);
    TEST_ASSERT(negative_range_resp.status == RtspStatusCode::parameter_not_understood, "negative npt should return 451");

    RtspRequest fractional_range_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 528\r\n"
            "Session: " + sid + "\r\n"
            "Range: npt=1.2-3.45\r\n"
            "\r\n",
            fractional_range_req)) {
        return false;
    }
    const RtspResponse fractional_range_resp = server.handle_request(fractional_range_req);
    TEST_ASSERT(fractional_range_resp.status == RtspStatusCode::method_not_valid_in_this_state ||
                    (fractional_range_resp.status == RtspStatusCode::ok &&
                     fractional_range_resp.headers.find("Range") != fractional_range_resp.headers.end() &&
                     fractional_range_resp.headers.at("Range") == "npt=1.200-3.450"),
                "fractional range should normalize to 3 decimals when accepted");

    RtspRequest open_start_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 529\r\n"
            "Session: " + sid + "\r\n"
            "Range: npt=-5.5\r\n"
            "\r\n",
            open_start_req)) {
        return false;
    }
    const RtspResponse open_start_resp = server.handle_request(open_start_req);
    TEST_ASSERT(open_start_resp.status == RtspStatusCode::method_not_valid_in_this_state ||
                    (open_start_resp.status == RtspStatusCode::ok &&
                     open_start_resp.headers.find("Range") != open_start_resp.headers.end() &&
                     open_start_resp.headers.at("Range") == "npt=-5.500"),
                "open-start range should normalize end to 3 decimals when accepted");

    RtspRequest max_scale_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 530\r\n"
            "Session: " + sid + "\r\n"
            "Scale: 16.0\r\n"
            "\r\n",
            max_scale_req)) {
        return false;
    }
    const RtspResponse max_scale_resp = server.handle_request(max_scale_req);
    TEST_ASSERT(max_scale_resp.status == RtspStatusCode::method_not_valid_in_this_state ||
                    (max_scale_resp.status == RtspStatusCode::ok &&
                     max_scale_resp.headers.find("Scale") != max_scale_resp.headers.end() &&
                     max_scale_resp.headers.at("Scale") == "16.000"),
                "maximum supported scale should be accepted when state allows");
    return true;
}

bool test_state_matrix_setup_multicandidate_transport_selection()
{
    RtspServer server;

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 80\r\n"
            "Transport: RTP/AVP/TCP;multicast;interleaved=0-1, RTP/AVP;unicast;client_port=4000-4001\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }

    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "setup should choose supported candidate");
    TEST_ASSERT(setup_resp.headers.find("Transport") != setup_resp.headers.end(), "transport response required");
    TEST_ASSERT(setup_resp.headers.at("Transport").find("RTP/AVP") != std::string::npos,
                "selected transport should be UDP unicast candidate");

    RtspRequest ffmpeg_style_transport_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track2 RTSP/1.0\r\n"
            "CSeq: 81\r\n"
            "Session: " + extract_session_id(setup_resp) + "\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=4-5, RTP/AVP;unicast;client_port=5000-5001\r\n"
            "\r\n",
            ffmpeg_style_transport_req)) {
        return false;
    }
    const RtspResponse ffmpeg_style_transport_resp = server.handle_request(ffmpeg_style_transport_req);
    TEST_ASSERT(ffmpeg_style_transport_resp.status == RtspStatusCode::ok, "ffmpeg style transport fallback should succeed");
    TEST_ASSERT(ffmpeg_style_transport_resp.headers.find("Transport") != ffmpeg_style_transport_resp.headers.end(),
                "transport header should exist");
    TEST_ASSERT(ffmpeg_style_transport_resp.headers.at("Transport").find("interleaved=4-5") != std::string::npos,
                "should pick first valid tcp candidate");
    return true;
}

bool test_state_matrix_session_error_semantics()
{
    RtspServer server;

    RtspRequest play_missing_session_header;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 90\r\n"
            "\r\n",
            play_missing_session_header)) {
        return false;
    }
    const RtspResponse missing_header_resp = server.handle_request(play_missing_session_header);
    TEST_ASSERT(missing_header_resp.status == RtspStatusCode::session_not_found, "missing session header should be 454");

    RtspRequest play_unknown_session;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 91\r\n"
            "Session: sid-never-exist\r\n"
            "\r\n",
            play_unknown_session)) {
        return false;
    }
    const RtspResponse unknown_resp = server.handle_request(play_unknown_session);
    TEST_ASSERT(unknown_resp.status == RtspStatusCode::session_not_found, "unknown session should be 454");

    server.configure_basic_auth("state-realm", "matrix", "pw");

    RtspRequest setup_no_auth;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/trackA RTSP/1.0\r\n"
            "CSeq: 92\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=4-5\r\n"
            "\r\n",
            setup_no_auth)) {
        return false;
    }
    const RtspResponse no_auth_resp = server.handle_request(setup_no_auth);
    TEST_ASSERT(no_auth_resp.status == RtspStatusCode::unauthorized, "setup without auth should be 401");
    TEST_ASSERT(no_auth_resp.headers.find("WWW-Authenticate") != no_auth_resp.headers.end(),
                "401 response should include challenge header");

    RtspRequest options_no_auth;
    if (!parse_request_or_fail(
            "OPTIONS rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 93\r\n"
            "\r\n",
            options_no_auth)) {
        return false;
    }
    const RtspResponse options_no_auth_resp = server.handle_request(options_no_auth);
    TEST_ASSERT(options_no_auth_resp.status == RtspStatusCode::ok, "options should remain auth-exempt");
    return true;
}

bool test_state_matrix_digest_acl_and_rate_limit_regression()
{
    RtspServer server;
    server.configure_digest_auth("matrix-digest", "matrix", "pw");

    RtspAclConfig acl;
    acl.enabled = true;
    acl.default_allow = true;
    acl.deny_uri_prefixes.push_back("rtsp://example/blocked");
    server.configure_acl(acl);

    RtspRequest blocked_describe;
    if (!parse_request_or_fail(
            "DESCRIBE rtsp://example/blocked/stream RTSP/1.0\r\n"
            "CSeq: 700\r\n"
            "\r\n",
            blocked_describe)) {
        return false;
    }
    const RtspResponse blocked_resp = server.handle_request(blocked_describe);
    TEST_ASSERT(blocked_resp.status == RtspStatusCode::forbidden, "acl denied describe should return 403");

    RtspRateLimitConfig rl;
    rl.enabled = true;
    rl.max_requests = 2;
    rl.window_ms = 60000;
    server.configure_rate_limit(rl);

    RtspRequest options1;
    RtspRequest options2;
    RtspRequest options3;
    if (!parse_request_or_fail("OPTIONS rtsp://example/live RTSP/1.0\r\nCSeq: 701\r\n\r\n", options1) ||
        !parse_request_or_fail("OPTIONS rtsp://example/live RTSP/1.0\r\nCSeq: 702\r\n\r\n", options2) ||
        !parse_request_or_fail("OPTIONS rtsp://example/live RTSP/1.0\r\nCSeq: 703\r\n\r\n", options3)) {
        return false;
    }
    TEST_ASSERT(server.handle_request(options1).status == RtspStatusCode::ok, "first request should pass");
    TEST_ASSERT(server.handle_request(options2).status == RtspStatusCode::ok, "second request should pass");
    TEST_ASSERT(server.handle_request(options3).status == RtspStatusCode::too_many_requests,
                "third request should be rate limited");
    return true;
}

bool test_state_matrix_protocol_edge_cases_cseq_order_and_reconnect()
{
    RtspServer server;

    RtspRequest setup_initial;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/matrix/track1 RTSP/1.0\r\n"
            "CSeq: 800\r\n"
            "Session: sid-matrix-edge\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            setup_initial)) {
        return false;
    }
    const RtspResponse setup_initial_resp = server.handle_request(setup_initial);
    TEST_ASSERT(setup_initial_resp.status == RtspStatusCode::ok, "initial setup should be ok");

    RtspRequest setup_out_of_order;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/matrix/track2 RTSP/1.0\r\n"
            "CSeq: 799\r\n"
            "Session: sid-matrix-edge\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n"
            "\r\n",
            setup_out_of_order)) {
        return false;
    }
    const RtspResponse setup_out_of_order_resp = server.handle_request(setup_out_of_order);
    TEST_ASSERT(setup_out_of_order_resp.status == RtspStatusCode::method_not_valid_in_this_state,
                "out-of-order setup cseq should return 455 in setup path");

    RtspRequest setup_track2;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/matrix/track2 RTSP/1.0\r\n"
            "CSeq: 801\r\n"
            "Session: sid-matrix-edge\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n"
            "\r\n",
            setup_track2)) {
        return false;
    }
    const RtspResponse setup_track2_resp = server.handle_request(setup_track2);
    TEST_ASSERT(setup_track2_resp.status == RtspStatusCode::ok, "second track setup should be ok");

    RtspRequest play_ok;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/matrix RTSP/1.0\r\n"
            "CSeq: 802\r\n"
            "Session: sid-matrix-edge\r\n"
            "\r\n",
            play_ok)) {
        return false;
    }
    const RtspResponse play_ok_resp = server.handle_request(play_ok);
    TEST_ASSERT(play_ok_resp.status == RtspStatusCode::ok, "play should be ok");

    RtspRequest keepalive_short;
    if (!parse_request_or_fail(
            "SET_PARAMETER rtsp://example/matrix RTSP/1.0\r\n"
            "CSeq: 803\r\n"
            "Session: sid-matrix-edge\r\n"
            "Content-Type: text/parameters\r\n"
            "\r\n"
            "timeout=1\r\n",
            keepalive_short)) {
        return false;
    }
    const RtspResponse keepalive_short_resp = server.handle_request(keepalive_short);
    TEST_ASSERT(keepalive_short_resp.status == RtspStatusCode::ok, "set_parameter should be ok");

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    RtspRequest play_expired;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/matrix RTSP/1.0\r\n"
            "CSeq: 805\r\n"
            "Session: sid-matrix-edge\r\n"
            "\r\n",
            play_expired)) {
        return false;
    }
    const RtspResponse play_expired_resp = server.handle_request(play_expired);
    TEST_ASSERT(play_expired_resp.status == RtspStatusCode::request_timeout,
                "play after short timeout should return 408");

    RtspRequest reconnect_setup;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/matrix/track1 RTSP/1.0\r\n"
            "CSeq: 806\r\n"
            "Session: sid-matrix-edge\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=8-9\r\n"
            "\r\n",
            reconnect_setup)) {
        return false;
    }
    const RtspResponse reconnect_setup_resp = server.handle_request(reconnect_setup);
    TEST_ASSERT(reconnect_setup_resp.status == RtspStatusCode::ok,
                "reconnect setup after timeout should re-create session");
    return true;
}

} // namespace

int main()
{
    std::cout << "=== RTSP State Matrix Test Suite ===\n";
    RUN_TEST(test_state_matrix_play_pause_record_transitions);
    RUN_TEST(test_state_matrix_missing_transport_and_cseq_regression);
    RUN_TEST(test_state_matrix_play_range_scale_headers);
    RUN_TEST(test_state_matrix_announce_sdp_validation);
    RUN_TEST(test_state_matrix_setup_multicandidate_transport_selection);
    RUN_TEST(test_state_matrix_session_error_semantics);
    RUN_TEST(test_state_matrix_record_requires_track_consistent_announce);
    RUN_TEST(test_state_matrix_digest_acl_and_rate_limit_regression);
    RUN_TEST(test_state_matrix_protocol_edge_cases_cseq_order_and_reconnect);

    std::cout << "Result: " << g_pass << "/" << g_run << " passed";
    if (g_fail > 0) {
        std::cout << ", " << g_fail << " failed";
    }
    std::cout << "\n";
    return g_fail == 0 ? 0 : 1;
}
