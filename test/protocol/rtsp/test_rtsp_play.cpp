#include "rtsp_parser.h"
#include "rtsp_protocol.h"
#include "rtsp_request.h"
#include "rtsp_response.h"

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

bool test_rtsp_play_request_parse_and_response_encode()
{
    const std::string raw_play =
        "PLAY rtsp://camera/live RTSP/1.0\r\n"
        "CSeq: 22\r\n"
        "Session: ABCD1234\r\n"
        "\r\n";

    RtspRequest request;
    TEST_ASSERT(RtspParser::parse_request(raw_play, request), "play request should parse");
    TEST_ASSERT(request.method == RtspMethod::play, "method should be play");
    TEST_ASSERT(request.cseq == 22, "cseq should be 22");
    TEST_ASSERT(request.header("Session") != nullptr, "session header should exist");

    RtspResponse response;
    response.status = RtspStatusCode::ok;
    response.cseq = request.cseq;
    response.headers["Session"] = "ABCD1234";
    response.headers["RTP-Info"] = "url=rtsp://camera/live;seq=0;rtptime=0";

    const std::string encoded = RtspParser::serialize_response(response);
    TEST_ASSERT(encoded.find("RTSP/1.0 200 OK") == 0, "status line should be 200");
    TEST_ASSERT(encoded.find("Session: ABCD1234") != std::string::npos, "session should be encoded");
    TEST_ASSERT(encoded.find("RTP-Info:") != std::string::npos, "rtp-info should be encoded");
    return true;
}

bool test_rtsp_state_related_status_strings()
{
    TEST_ASSERT(std::string(status_code_reason(RtspStatusCode::session_not_found)) == "Session Not Found", "454 string should match");
    TEST_ASSERT(std::string(status_code_reason(RtspStatusCode::method_not_valid_in_this_state)) == "Method Not Valid In This State", "455 string should match");
    return true;
}

} // namespace

int main()
{
    std::cout << "=== RTSP Play Test Suite ===\n";
    RUN_TEST(test_rtsp_play_request_parse_and_response_encode);
    RUN_TEST(test_rtsp_state_related_status_strings);

    std::cout << "Result: " << g_pass << "/" << g_run << " passed";
    if (g_fail > 0) {
        std::cout << ", " << g_fail << " failed";
    }
    std::cout << "\n";
    return g_fail == 0 ? 0 : 1;
}
