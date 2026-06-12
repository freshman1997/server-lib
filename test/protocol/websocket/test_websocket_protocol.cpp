#include "common/websocket_connection.h"
#include "common/websocket_config.h"
#include "common/websocket_protocol.h"

#include "buffer/byte_buffer.h"

#include <cstdint>
#include <iostream>
#include <limits>
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

ByteBuffer make_frame_with_len_code(std::string_view payload,
                                    uint8_t opcode,
                                    bool masked,
                                    uint8_t len_code,
                                    uint64_t extended_len,
                                    bool fin = true)
{
    ByteBuffer frame(2 + 8 + 4 + payload.size());
    frame.append_u8(static_cast<uint8_t>((fin ? 0x80 : 0x00) | (opcode & 0x0f)));
    frame.append_u8(static_cast<uint8_t>((masked ? 0x80 : 0x00) | len_code));
    if (len_code == 126) {
        frame.append_u16(static_cast<uint16_t>(extended_len));
    } else if (len_code == 127) {
        frame.append_u64(extended_len);
    }

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

bool test_rejects_fragmented_control_frame_with_payload()
{
    WebSocketConnection conn(WorkMode::server_);
    auto frame = make_frame("x", static_cast<uint8_t>(OpCodeType::type_ping_frame), true, false);

    TEST_ASSERT(!conn.pkt_parser().unpack_from(&conn, frame), "fragmented control frame with payload must be rejected");
    return true;
}

bool test_rejects_control_payload_over_125_bytes()
{
    WebSocketConnection conn(WorkMode::server_);
    const std::string payload(126, 'x');
    auto frame = make_frame_with_len_code(payload,
                                          static_cast<uint8_t>(OpCodeType::type_ping_frame),
                                          true,
                                          126,
                                          payload.size());

    TEST_ASSERT(!conn.pkt_parser().unpack_from(&conn, frame), "control payload over 125 bytes must be rejected");
    return true;
}

bool test_rejects_non_minimal_16bit_payload_length()
{
    WebSocketConnection conn(WorkMode::server_);
    auto frame = make_frame_with_len_code(std::string(125, 'x'),
                                          static_cast<uint8_t>(OpCodeType::type_binary_frame),
                                          true,
                                          websocket_payload_len_16bit_marker,
                                          websocket_payload_len_7bit_max);

    TEST_ASSERT(!conn.pkt_parser().unpack_from(&conn, frame), "payload <= 125 must not use 16-bit length encoding");
    return true;
}

bool test_accepts_minimal_16bit_payload_length_boundary()
{
    WebSocketConnection conn(WorkMode::server_);
    const std::string payload(websocket_payload_len_7bit_max + 1, 'x');
    auto frame = make_frame_with_len_code(payload,
                                          static_cast<uint8_t>(OpCodeType::type_binary_frame),
                                          true,
                                          websocket_payload_len_16bit_marker,
                                          payload.size());

    TEST_ASSERT(conn.pkt_parser().unpack_from(&conn, frame), "payload 126 should use 16-bit length encoding");
    TEST_ASSERT(conn.input_chunks().size() == 1, "16-bit boundary payload should dispatch");
    TEST_ASSERT(conn.input_chunks()[0].body_.readable_bytes() == payload.size(), "16-bit boundary payload size should match");
    return true;
}

bool test_accepts_max_16bit_payload_length_boundary()
{
    WebSocketConnection conn(WorkMode::server_);
    const std::string payload(websocket_payload_len_16bit_max, 'x');
    auto frame = make_frame_with_len_code(payload,
                                          static_cast<uint8_t>(OpCodeType::type_binary_frame),
                                          true,
                                          websocket_payload_len_16bit_marker,
                                          payload.size());

    TEST_ASSERT(conn.pkt_parser().unpack_from(&conn, frame), "payload 65535 should use 16-bit length encoding");
    TEST_ASSERT(conn.input_chunks().size() == 1, "max 16-bit payload should dispatch");
    TEST_ASSERT(conn.input_chunks()[0].body_.readable_bytes() == payload.size(), "max 16-bit payload size should match");
    return true;
}

bool test_rejects_non_minimal_64bit_payload_length()
{
    WebSocketConnection conn(WorkMode::server_);
    const std::string payload(websocket_payload_len_16bit_max, 'x');
    auto frame = make_frame_with_len_code(payload,
                                          static_cast<uint8_t>(OpCodeType::type_binary_frame),
                                          true,
                                          websocket_payload_len_64bit_marker,
                                          payload.size());

    TEST_ASSERT(!conn.pkt_parser().unpack_from(&conn, frame), "payload <= 65535 must not use 64-bit length encoding");
    return true;
}

bool test_accepts_minimal_64bit_payload_length_boundary()
{
    WebSocketConnection conn(WorkMode::server_);
    const std::string payload(websocket_payload_len_16bit_max + 1, 'x');
    auto frame = make_frame_with_len_code(payload,
                                          static_cast<uint8_t>(OpCodeType::type_binary_frame),
                                          true,
                                          websocket_payload_len_64bit_marker,
                                          payload.size());

    TEST_ASSERT(conn.pkt_parser().unpack_from(&conn, frame), "payload 65536 should use 64-bit length encoding");
    TEST_ASSERT(conn.input_chunks().size() == 1, "64-bit boundary payload should dispatch");
    TEST_ASSERT(conn.input_chunks()[0].body_.readable_bytes() == payload.size(), "64-bit boundary payload size should match");
    return true;
}

bool test_rejects_64bit_payload_with_high_bit_set()
{
    WebSocketConnection conn(WorkMode::server_);
    auto frame = make_frame_with_len_code("",
                                          static_cast<uint8_t>(OpCodeType::type_binary_frame),
                                          true,
                                          websocket_payload_len_64bit_marker,
                                          websocket_payload_len_64bit_msb);

    TEST_ASSERT(!conn.pkt_parser().unpack_from(&conn, frame), "64-bit payload length must not set the high bit");
    return true;
}

bool test_rejects_interleaved_fragmented_data_message()
{
    WebSocketConnection conn(WorkMode::server_);
    auto first = make_frame("a", static_cast<uint8_t>(OpCodeType::type_text_frame), true, false);
    auto second = make_frame("b", static_cast<uint8_t>(OpCodeType::type_binary_frame), true, true);

    TEST_ASSERT(conn.pkt_parser().unpack_from(&conn, first), "first fragment should parse");
    TEST_ASSERT(!conn.pkt_parser().unpack_from(&conn, second), "new data frame before continuation must be rejected");
    return true;
}

bool test_rejects_fragmented_message_over_limit()
{
    WebSocketConfigManager config;
    TEST_ASSERT(config.init(true), "server config should load");

    WebSocketConnection conn(WorkMode::server_);
    conn.set_config(&config);
    auto first = make_frame(std::string(64, 'a'), static_cast<uint8_t>(OpCodeType::type_text_frame), true, false);
    auto second = make_frame(std::string(64, 'b'), static_cast<uint8_t>(OpCodeType::type_continue_frame), true, true);

    conn.set_outbound_queue_limits(0, 0);
    TEST_ASSERT(conn.pkt_parser().unpack_from(&conn, first), "first fragment should parse");
    TEST_ASSERT(conn.input_chunks().empty(), "first fragment should not dispatch yet");

    // Direct config override is not exposed, so use the parser's hard limit by crafting an impossible final size.
    auto huge = make_frame_with_len_code("",
                                         static_cast<uint8_t>(OpCodeType::type_continue_frame),
                                         true,
                                         127,
                                         static_cast<uint64_t>(PACKET_MAX_BYTE) + 1);
    TEST_ASSERT(!conn.pkt_parser().unpack_from(&conn, huge), "fragment exceeding packet limit must be rejected");
    return true;
}

bool test_rejects_invalid_close_payload_length()
{
    WebSocketConnection conn(WorkMode::server_);
    auto frame = make_frame("x", static_cast<uint8_t>(OpCodeType::type_close_frame), true);

    TEST_ASSERT(!conn.pkt_parser().unpack_from(&conn, frame), "close payload with one byte must be rejected");
    return true;
}

bool test_rejects_invalid_close_code()
{
    WebSocketConnection conn(WorkMode::server_);
    const std::string payload{"\x03\xed", 2}; // 1005 is reserved and cannot appear on the wire.
    auto frame = make_frame(payload, static_cast<uint8_t>(OpCodeType::type_close_frame), true);

    TEST_ASSERT(!conn.pkt_parser().unpack_from(&conn, frame), "reserved close code must be rejected");
    return true;
}

bool test_accepts_valid_close_code()
{
    WebSocketConnection conn(WorkMode::server_);
    const std::string payload{"\x03\xe8", 2}; // 1000 normal closure.
    auto frame = make_frame(payload, static_cast<uint8_t>(OpCodeType::type_close_frame), true);

    TEST_ASSERT(conn.pkt_parser().unpack_from(&conn, frame), "valid close code should parse");
    TEST_ASSERT(conn.input_chunks().size() == 1, "valid close frame should dispatch");
    TEST_ASSERT(conn.input_chunks()[0].head_.is_close_frame(), "dispatched frame should be close");
    return true;
}

bool test_rejects_invalid_text_utf8()
{
    WebSocketConnection conn(WorkMode::server_);
    const std::string payload{"\xc0\xaf", 2}; // Overlong encoding is invalid UTF-8.
    auto frame = make_frame(payload, static_cast<uint8_t>(OpCodeType::type_text_frame), true);

    TEST_ASSERT(!conn.pkt_parser().unpack_from(&conn, frame), "invalid text UTF-8 must be rejected");
    return true;
}

bool test_binary_allows_non_utf8_payload()
{
    WebSocketConnection conn(WorkMode::server_);
    const std::string payload{"\xc0\xaf", 2};
    auto frame = make_frame(payload, static_cast<uint8_t>(OpCodeType::type_binary_frame), true);

    TEST_ASSERT(conn.pkt_parser().unpack_from(&conn, frame), "binary payload is not UTF-8 validated");
    TEST_ASSERT(conn.input_chunks().size() == 1, "binary message should dispatch");
    return true;
}

bool test_rejects_fragmented_invalid_text_utf8()
{
    WebSocketConnection conn(WorkMode::server_);
    const std::string firstPayload{"\xc0", 1};
    const std::string secondPayload{"\xaf", 1};
    auto first = make_frame(firstPayload, static_cast<uint8_t>(OpCodeType::type_text_frame), true, false);
    auto second = make_frame(secondPayload, static_cast<uint8_t>(OpCodeType::type_continue_frame), true, true);

    TEST_ASSERT(conn.pkt_parser().unpack_from(&conn, first), "first fragment should wait for continuation");
    TEST_ASSERT(!conn.pkt_parser().unpack_from(&conn, second), "reassembled invalid UTF-8 must be rejected");
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
    RUN_TEST(test_rejects_fragmented_control_frame_with_payload);
    RUN_TEST(test_rejects_control_payload_over_125_bytes);
    RUN_TEST(test_rejects_non_minimal_16bit_payload_length);
    RUN_TEST(test_accepts_minimal_16bit_payload_length_boundary);
    RUN_TEST(test_accepts_max_16bit_payload_length_boundary);
    RUN_TEST(test_rejects_non_minimal_64bit_payload_length);
    RUN_TEST(test_accepts_minimal_64bit_payload_length_boundary);
    RUN_TEST(test_rejects_64bit_payload_with_high_bit_set);
    RUN_TEST(test_rejects_interleaved_fragmented_data_message);
    RUN_TEST(test_rejects_fragmented_message_over_limit);
    RUN_TEST(test_rejects_invalid_close_payload_length);
    RUN_TEST(test_rejects_invalid_close_code);
    RUN_TEST(test_accepts_valid_close_code);
    RUN_TEST(test_rejects_invalid_text_utf8);
    RUN_TEST(test_binary_allows_non_utf8_payload);
    RUN_TEST(test_rejects_fragmented_invalid_text_utf8);
    RUN_TEST(test_client_mask_is_default_for_packing);

    std::cout << "\nResults: " << g_pass << " passed, " << g_fail << " failed, " << g_run << " total\n";
    return g_fail == 0 ? 0 : 1;
}
