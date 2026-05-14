#include "rtc_packet.h"

#include "buffer/byte_buffer.h"

#include <cstdint>
#include <iostream>
#include <string>

using namespace yuan::net::rtc;
using namespace yuan::buffer;

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

bool test_rtc_basic_roundtrip()
{
    RtcPacket packet;
    packet.marker = true;
    packet.payload_type = 111;
    packet.sequence_number = 3456;
    packet.timestamp = 0x12345678u;
    packet.ssrc = 0xABCDEF01u;
    packet.payload = {1, 2, 3, 4, 5};

    ByteBuffer buf;
    TEST_ASSERT(packet.serialize(buf), "serialize should succeed");

    RtcPacket parsed;
    TEST_ASSERT(parsed.deserialize(buf), "deserialize should succeed");

    TEST_ASSERT(parsed.version == 2, "version should be 2");
    TEST_ASSERT(parsed.marker, "marker should be true");
    TEST_ASSERT(parsed.payload_type == 111, "payload_type should match");
    TEST_ASSERT(parsed.sequence_number == 3456, "sequence should match");
    TEST_ASSERT(parsed.timestamp == 0x12345678u, "timestamp should match");
    TEST_ASSERT(parsed.ssrc == 0xABCDEF01u, "ssrc should match");
    TEST_ASSERT(parsed.payload == packet.payload, "payload should match");
    return true;
}

bool test_rtc_extension_roundtrip()
{
    RtcPacket packet;
    packet.extension = true;
    packet.extension_profile = 0xBEDE;
    packet.extension_data = {0x10, 0x20, 0x30, 0x40};
    packet.payload = {0xAA, 0xBB, 0xCC};

    ByteBuffer buf;
    TEST_ASSERT(packet.serialize(buf), "serialize should succeed");

    RtcPacket parsed;
    TEST_ASSERT(parsed.deserialize(buf), "deserialize should succeed");

    TEST_ASSERT(parsed.extension, "extension should be true");
    TEST_ASSERT(parsed.extension_profile == 0xBEDE, "extension profile should match");
    TEST_ASSERT(parsed.extension_data == packet.extension_data, "extension data should match");
    TEST_ASSERT(parsed.payload == packet.payload, "payload should match");
    return true;
}

bool test_rtc_padding_roundtrip()
{
    RtcPacket packet;
    packet.padding = true;
    packet.padding_size = 4;
    packet.payload = {'t', 'e', 's', 't'};

    ByteBuffer buf;
    TEST_ASSERT(packet.serialize(buf), "serialize should succeed");

    RtcPacket parsed;
    TEST_ASSERT(parsed.deserialize(buf), "deserialize should succeed");
    TEST_ASSERT(parsed.padding, "padding should be true");
    TEST_ASSERT(parsed.padding_size == 4, "padding_size should match");
    TEST_ASSERT(parsed.payload == packet.payload, "payload should match");
    return true;
}

} // namespace

int main()
{
    std::cout << "=== RTC Protocol Test Suite ===\n";
    RUN_TEST(test_rtc_basic_roundtrip);
    RUN_TEST(test_rtc_extension_roundtrip);
    RUN_TEST(test_rtc_padding_roundtrip);

    std::cout << "Result: " << g_pass << "/" << g_run << " passed";
    if (g_fail > 0) {
        std::cout << ", " << g_fail << " failed";
    }
    std::cout << "\n";
    return g_fail == 0 ? 0 : 1;
}
