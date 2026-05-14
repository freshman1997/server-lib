#include "rtsp_framing.h"

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

bool test_framer_half_and_sticky_rtsp_frames()
{
    RtspStreamFramer framer;
    RtspFrame frame;

    framer.push("OPTIONS rtsp://example/live RTSP/1.0\r\nCSeq: 1\r\n\r\nDES");
    TEST_ASSERT(framer.pop(frame) == RtspFrameParseResult::ok, "first frame should parse");
    TEST_ASSERT(frame.kind == RtspFrameKind::rtsp_request, "first frame should be rtsp request");
    TEST_ASSERT(frame.data.find("OPTIONS") == 0, "first request should be options");

    TEST_ASSERT(framer.pop(frame) == RtspFrameParseResult::need_more, "second frame should be incomplete");
    framer.push("CRIBE rtsp://example/live RTSP/1.0\r\nCSeq: 2\r\n\r\n");
    TEST_ASSERT(framer.pop(frame) == RtspFrameParseResult::ok, "second frame should parse after append");
    TEST_ASSERT(frame.data.find("DESCRIBE") == 0, "second request should be describe");
    return true;
}

bool test_framer_rtsp_with_content_length_body_split()
{
    RtspStreamFramer framer;
    RtspFrame frame;

    framer.push("ANNOUNCE rtsp://example/live RTSP/1.0\r\nCSeq: 3\r\nContent-Length: 4\r\n\r\nAB");
    TEST_ASSERT(framer.pop(frame) == RtspFrameParseResult::need_more, "body half should wait");
    framer.push("CD");
    TEST_ASSERT(framer.pop(frame) == RtspFrameParseResult::ok, "full body should parse");
    TEST_ASSERT(frame.data.find("ABCD") != std::string::npos, "body should be preserved");
    return true;
}

bool test_framer_interleaved_half_and_sticky_frames()
{
    RtspStreamFramer framer;
    RtspFrame frame;

    const std::string p1 = "abc";
    const std::string p2 = "de";
    std::string interleaved;
    interleaved.push_back('$');
    interleaved.push_back(static_cast<char>(4));
    interleaved.push_back(0);
    interleaved.push_back(static_cast<char>(p1.size()));
    interleaved += p1;

    std::string interleaved2;
    interleaved2.push_back('$');
    interleaved2.push_back(static_cast<char>(5));
    interleaved2.push_back(0);
    interleaved2.push_back(static_cast<char>(p2.size()));
    interleaved2 += p2;

    framer.push(interleaved.substr(0, 5));
    TEST_ASSERT(framer.pop(frame) == RtspFrameParseResult::need_more, "half interleaved should wait");
    framer.push(interleaved.substr(5) + interleaved2);

    TEST_ASSERT(framer.pop(frame) == RtspFrameParseResult::ok, "first interleaved should parse");
    TEST_ASSERT(frame.kind == RtspFrameKind::interleaved, "frame kind should be interleaved");
    TEST_ASSERT(frame.channel == 4, "channel should match");
    TEST_ASSERT(frame.data == p1, "payload should match first interleaved");

    TEST_ASSERT(framer.pop(frame) == RtspFrameParseResult::ok, "second interleaved should parse");
    TEST_ASSERT(frame.channel == 5, "second channel should match");
    TEST_ASSERT(frame.data == p2, "payload should match second interleaved");
    return true;
}

bool test_framer_malformed_content_length()
{
    RtspStreamFramer framer;
    RtspFrame frame;

    framer.push("ANNOUNCE rtsp://example/live RTSP/1.0\r\nCSeq: 9\r\nContent-Length: bad\r\n\r\n");
    TEST_ASSERT(framer.pop(frame) == RtspFrameParseResult::malformed, "invalid content-length should be malformed");
    return true;
}

} // namespace

int main()
{
    std::cout << "=== RTSP Framing Test Suite ===\n";
    RUN_TEST(test_framer_half_and_sticky_rtsp_frames);
    RUN_TEST(test_framer_rtsp_with_content_length_body_split);
    RUN_TEST(test_framer_interleaved_half_and_sticky_frames);
    RUN_TEST(test_framer_malformed_content_length);

    std::cout << "Result: " << g_pass << "/" << g_run << " passed";
    if (g_fail > 0) {
        std::cout << ", " << g_fail << " failed";
    }
    std::cout << "\n";
    return g_fail == 0 ? 0 : 1;
}
