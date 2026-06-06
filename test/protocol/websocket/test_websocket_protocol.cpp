#include "common/websocket_connection.h"
#include "common/websocket_config.h"
#include "common/websocket_protocol.h"

#include "buffer/byte_buffer.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace yuan::buffer;
using namespace yuan::net::websocket;

namespace
{

int g_run = 0;
int g_pass = 0;
int g_fail = 0;

#define TEST_ASSERT(expr, msg)                                                            \
    do {                                                                                  \
        if (!(expr)) {                                                                    \
            std::cout << "  FAIL: " << msg << " (line " << __LINE__ << ")\n";          \
            return false;                                                                 \
        }                                                                                 \
    } while (0)

#define RUN_TEST(func)                                                                    \
    do {                                                                                  \
        ++g_run;                                                                          \
        std::cout << "Running " #func "...\n";                                          \
        if (func()) {                                                                     \
            ++g_pass;                                                                     \
            std::cout << "  PASS\n";                                                     \
        } else {                                                                          \
            ++g_fail;                                                                     \
            std::cout << "  FAIL\n";                                                     \
        }                                                                                 \
    } while (0)

ByteBuffer make_frame(std::string_view payload, uint8_t opcode, bool masked, bool fin = true, bool rsv1 = false)
{
    ByteBuffer frame(2 + 4 + payload.size());
    uint8_t first = opcode & 0x0f;
    if (fin) {
        first |= 0x80;
    }
    if (rsv1) {
        first |= 0x40;
    }
    frame.append_u8(first);
    frame.append_u8(static_cast<uint8_t>((masked ? 0x80 : 0x00) | payload.size()));

    const uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    if (masked) {
        for (uint8_t byte : mask) {
            frame.append_u8(byte);
        }
    }

    for (std::size_t i = 0; i < payload.size(); ++i) {
        const auto raw = static_cast<uint8_t>(payload[i]);
        frame.append_u8(masked ? static_cast<uint8_t>(raw ^ mask[i % 4]) : raw);
    }
    return frame;
}

std::string chunk_text(const ProtoChunk &chunk)
{
    return std::string(chunk.body_.read_ptr(), chunk.body_.readable_bytes());
}

bool test_server_accepts_masked_client_text()
{
    WebSocketConnection conn(WorkMode::server_);
    auto frame = make_frame("hello", static_cast<uint8_t>(OpCodeType::type_text_frame), true);

    TEST_ASSERT(conn.pkt_parser().unpack_from(&conn, frame), "masked client frame should parse");
    TEST_ASSERT(conn.input_chunks().size() == 1, "one message should be available");
    TEST_ASSERT(chunk_text(conn.input_chunks()[0]) == "hello", "payload should be unmasked");
    return true;
}

bool test_server_rejects_unmasked_client_text()
{
    WebSocketConnection conn(WorkMode::server_);
    auto frame = make_frame("hello", static_cast<uint8_t>(OpCodeType::type_text_frame), false);

    TEST_ASSERT(!conn.pkt_parser().unpack_from(&conn, frame), "server must reject unmasked client frame");
    return true;
}

bool test_client_rejects_masked_server_text()
{
    WebSocketConnection conn(WorkMode::client_);
    auto frame = make_frame("hello", static_cast<uint8_t>(OpCodeType::type_text_frame), true);

    TEST_ASSERT(!conn.pkt_parser().unpack_from(&conn, frame), "client must reject masked server frame");
    return true;
}

bool test_client_accepts_unmasked_server_text()
{
    WebSocketConnection conn(WorkMode::client_);
    auto frame = make_frame("hello", static_cast<uint8_t>(OpCodeType::type_text_frame), false);

    TEST_ASSERT(conn.pkt_parser().unpack_from(&conn, frame), "unmasked server frame should parse");
    TEST_ASSERT(conn.input_chunks().size() == 1, "one message should be available");
    TEST_ASSERT(chunk_text(conn.input_chunks()[0]) == "hello", "payload should match");
    return true;
}

bool test_rejects_rsv_without_extension()
{
    WebSocketConnection conn(WorkMode::server_);
    auto frame = make_frame("hello", static_cast<uint8_t>(OpCodeType::type_text_frame), true, true, true);

    TEST_ASSERT(!conn.pkt_parser().unpack_from(&conn, frame), "RSV frame should be rejected");
    return true;
}

bool test_rejects_unknown_opcode()
{
    WebSocketConnection conn(WorkMode::server_);
    auto frame = make_frame("hello", 0x03, true);

    TEST_ASSERT(!conn.pkt_parser().unpack_from(&conn, frame), "unknown opcode should be rejected");
    return true;
}

bool test_partial_frame_waits_for_more_data()
{
    WebSocketConnection conn(WorkMode::server_);
    auto frame = make_frame("hello", static_cast<uint8_t>(OpCodeType::type_text_frame), true);
    ByteBuffer first(frame.read_ptr(), 4);
    ByteBuffer second(frame.read_ptr() + 4, frame.readable_bytes() - 4);

    TEST_ASSERT(conn.pkt_parser().unpack_from(&conn, first), "partial frame should not fail");
    TEST_ASSERT(conn.input_chunks().empty(), "partial frame should not be dispatched");
    TEST_ASSERT(conn.pkt_parser().unpack_from(&conn, second), "remaining bytes should complete frame");
    TEST_ASSERT(conn.input_chunks().size() == 1, "completed frame should dispatch once");
    TEST_ASSERT(chunk_text(conn.input_chunks()[0]) == "hello", "payload should match");
    return true;
}

bool test_fragmented_message_reassembles()
{
    WebSocketConnection conn(WorkMode::server_);
    auto first = make_frame("he", static_cast<uint8_t>(OpCodeType::type_text_frame), true, false);
    auto second = make_frame("llo", static_cast<uint8_t>(OpCodeType::type_continue_frame), true, true);

    TEST_ASSERT(conn.pkt_parser().unpack_from(&conn, first), "first fragment should parse");
    TEST_ASSERT(conn.input_chunks().empty(), "unfinished message should not dispatch");
    TEST_ASSERT(conn.pkt_parser().unpack_from(&conn, second), "continuation should parse");
    TEST_ASSERT(conn.input_chunks().size() == 1, "completed message should dispatch");
    TEST_ASSERT(chunk_text(conn.input_chunks()[0]) == "hello", "fragmented payload should merge");
    return true;
}

bool test_rejects_fragmented_control_frame()
{
    WebSocketConnection conn(WorkMode::server_);
    auto frame = make_frame("", static_cast<uint8_t>(OpCodeType::type_ping_frame), true, false);

    TEST_ASSERT(!conn.pkt_parser().unpack_from(&conn, frame), "control frames must not be fragmented");
    return true;
}

bool test_client_mask_is_default_for_packing()
{
    WebSocketConfigManager config;
    TEST_ASSERT(config.init(false), "default client config should load");

    WebSocketConnection conn(WorkMode::client_);
    conn.set_config(&config);

    ByteBuffer payload(std::string_view("hi"));
    std::vector<ByteBuffer> output;
    TEST_ASSERT(conn.pack_frame(payload, static_cast<uint8_t>(OpCodeType::type_text_frame), output),
                "client should pack text frame");
    TEST_ASSERT(output.size() == 1, "one packed frame expected");
    TEST_ASSERT(output[0].readable_bytes() >= 2, "packed frame should have header");
    TEST_ASSERT((static_cast<uint8_t>(output[0].read_ptr()[1]) & 0x80) != 0, "client frame should be masked by default");
    return true;
}

} // namespace

int main()
{
    std::cout << "=== WebSocket Protocol Test Suite ===\n";
    RUN_TEST(test_server_accepts_masked_client_text);
    RUN_TEST(test_server_rejects_unmasked_client_text);
    RUN_TEST(test_client_rejects_masked_server_text);
    RUN_TEST(test_client_accepts_unmasked_server_text);
    RUN_TEST(test_rejects_rsv_without_extension);
    RUN_TEST(test_rejects_unknown_opcode);
    RUN_TEST(test_partial_frame_waits_for_more_data);
    RUN_TEST(test_fragmented_message_reassembles);
    RUN_TEST(test_rejects_fragmented_control_frame);
    RUN_TEST(test_client_mask_is_default_for_packing);

    std::cout << "\nResults: " << g_pass << " passed, " << g_fail << " failed, " << g_run << " total\n";
    return g_fail == 0 ? 0 : 1;
}
