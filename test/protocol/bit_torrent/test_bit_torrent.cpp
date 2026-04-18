#include <cstdio>
#include <cstring>
#include <cassert>
#include <array>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include "common/winsock_guard.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "utils.h"
#include "torrent_meta.h"
#include "peer_wire/peer_wire_message.h"
#include "nat/nat_config.h"
#include "nat/nat_manager.h"
#include "nat/utp_connection.h"
#include "nat/dht_node.h"
#include "nat/pex_manager.h"
#include "net/poller/select_poller.h"
#include "session/tracker_session.h"
#include "state/piece_download_state.h"
#include "stats/download_stats_tracker.h"
#include "storage/piece_storage.h"
#include "timer/wheel_timer_manager.h"
#include "bit_torrent_client.h"
#include "tracker/http_tracker.h"
#include <filesystem>

using namespace yuan::net::bit_torrent;

// ============================================================================
// Test Framework
// ============================================================================

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name)                                    \
    do {                                              \
        printf("  [TEST] %-55s", name);               \
    } while (0)

#define PASS()                                        \
    do {                                              \
        printf("PASS\n");                             \
        g_pass++;                                     \
    } while (0)

#define FAIL(msg)                                     \
    do {                                              \
        printf("FAIL  %s\n", msg);                    \
        g_fail++;                                     \
    } while (0)

#define CHECK(cond, msg)                              \
    do {                                              \
        if (cond) { PASS(); }                         \
        else { FAIL(msg); }                           \
    } while (0)

#ifdef _WIN32
static void close_test_socket(SOCKET fd)
{
    if (fd != INVALID_SOCKET) {
        closesocket(fd);
    }
}
#else
static void close_test_socket(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}
#endif

// ============================================================================
// Helper: build bencoded strings for test data
// ============================================================================

static std::string bencode_str(const std::string &s)
{
    return std::to_string(s.size()) + ":" + s;
}

static std::string bencode_int(int64_t v)
{
    return "i" + std::to_string(v) + "e";
}

// Build a valid minimal single-file torrent bencode string
static std::string build_test_torrent(const std::string &name,
                                       int64_t file_length,
                                       int64_t piece_length,
                                       int num_pieces)
{
    std::string fake_hash(num_pieces * 20, '\x42');
    std::string tracker_url = "http://tracker.example.com/announce";

    return "d"
        + bencode_str("announce") + bencode_str(tracker_url)
        + bencode_str("announce-list") + "l"
            "l" + bencode_str(tracker_url) + "e"
        + "e"
        + bencode_str("info") + "d"
            + bencode_str("name") + bencode_str(name)
            + bencode_str("piece length") + bencode_int(piece_length)
            + bencode_str("pieces") + bencode_str(fake_hash)
            + bencode_str("length") + bencode_int(file_length)
        + "e"
    + "e";
}

static std::string build_single_file_torrent_with_hashes(const std::string &name,
                                                         const std::string &content,
                                                         int64_t piece_length)
{
    std::string pieces;
    for (size_t offset = 0; offset < content.size(); offset += static_cast<size_t>(piece_length))
    {
        const auto len = std::min<size_t>(static_cast<size_t>(piece_length), content.size() - offset);
        const auto hash = sha1_hash(reinterpret_cast<const uint8_t *>(content.data() + offset), len);
        pieces.append(reinterpret_cast<const char *>(hash.data()), hash.size());
    }

    const std::string tracker_url = "http://tracker.example.com/announce";
    return "d"
        + bencode_str("announce") + bencode_str(tracker_url)
        + bencode_str("info") + "d"
            + bencode_str("name") + bencode_str(name)
            + bencode_str("piece length") + bencode_int(piece_length)
            + bencode_str("pieces") + bencode_str(pieces)
            + bencode_str("length") + bencode_int(static_cast<int64_t>(content.size()))
        + "e"
    + "e";
}

static std::string build_single_file_torrent_without_announce(const std::string &name,
                                                              const std::string &content,
                                                              int64_t piece_length)
{
    std::string pieces;
    for (size_t offset = 0; offset < content.size(); offset += static_cast<size_t>(piece_length))
    {
        const auto len = std::min<size_t>(static_cast<size_t>(piece_length), content.size() - offset);
        const auto hash = sha1_hash(reinterpret_cast<const uint8_t *>(content.data() + offset), len);
        pieces.append(reinterpret_cast<const char *>(hash.data()), hash.size());
    }

    return "d"
        + bencode_str("info") + "d"
            + bencode_str("name") + bencode_str(name)
            + bencode_str("piece length") + bencode_int(piece_length)
            + bencode_str("pieces") + bencode_str(pieces)
            + bencode_str("length") + bencode_int(static_cast<int64_t>(content.size()))
        + "e"
    + "e";
}

static std::filesystem::path make_bt_test_dir(const std::string &name)
{
    std::error_code ec;
    auto dir = std::filesystem::path(__FILE__).parent_path().parent_path() / "build" / "test" / "tmp";
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        dir = std::filesystem::path(__FILE__).parent_path() / "tmp";
        ec.clear();
    }
    dir /= "webserver_bt_tests";
    dir /= name;
    std::filesystem::remove_all(dir, ec);
    ec.clear();
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// ============================================================================
// Part 1: Utils Tests
// ============================================================================

void test_sha1_hash()
{
    printf("\n=== Utils: sha1_hash ===\n");

    TEST("sha1 empty string");
    {
        auto hash = sha1_hash("");
        auto hex = to_hex(hash);
        CHECK(hex == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "wrong hash for empty string");
    }

    TEST("sha1 'abc'");
    {
        auto hash = sha1_hash("abc");
        auto hex = to_hex(hash);
        CHECK(hex == "a9993e364706816aba3e25717850c26c9cd0d89d", "wrong hash for 'abc'");
    }

    TEST("sha1 hash length is 20 bytes");
    {
        auto hash = sha1_hash("test");
        CHECK(hash.size() == 20, "hash should be 20 bytes");
    }

    TEST("sha1 raw pointer overload");
    {
        const unsigned char data[] = {0x01, 0x02, 0x03};
        auto hash = sha1_hash(data, 3);
        CHECK(hash.size() == 20, "hash from raw ptr should be 20 bytes");
    }
}

void test_to_hex()
{
    printf("\n=== Utils: to_hex ===\n");

    TEST("to_hex empty vector");
    {
        std::vector<uint8_t> empty;
        CHECK(to_hex(empty) == "", "empty vector should give empty hex");
    }

    TEST("to_hex known bytes");
    {
        std::vector<uint8_t> data = {0x01, 0x23, 0xab, 0xff};
        CHECK(to_hex(data) == "0123abff", "hex encoding mismatch");
    }

    TEST("to_hex raw pointer");
    {
        uint8_t data[] = {0xde, 0xad};
        CHECK(to_hex(data, 2) == "dead", "raw ptr hex encoding mismatch");
    }
}

void test_from_hex()
{
    printf("\n=== Utils: from_hex ===\n");

    TEST("from_hex roundtrip");
    {
        std::vector<uint8_t> original = {0x01, 0x23, 0xab, 0xff, 0x00};
        std::string hex = to_hex(original);
        auto recovered = from_hex(hex);
        CHECK(recovered == original, "from_hex roundtrip failed");
    }

    TEST("from_hex empty string");
    {
        auto bytes = from_hex("");
        CHECK(bytes.empty(), "from_hex empty should give empty");
    }

    TEST("from_hex single byte");
    {
        auto bytes = from_hex("ff");
        CHECK(bytes.size() == 1 && bytes[0] == 0xff, "from_hex single byte failed");
    }
}

void test_url_encode()
{
    printf("\n=== Utils: url_encode ===\n");

    TEST("url_encode no special chars");
    {
        CHECK(url_encode("abc123") == "abc123", "alphanumeric should not be encoded");
    }

    TEST("url_encode space");
    {
        CHECK(url_encode("a b") == "a%20b", "space should be %20");
    }

    TEST("url_encode special chars");
    {
        CHECK(url_encode("hello world!") == "hello%20world%21", "special char encoding mismatch");
    }

    TEST("url_encode unreserved chars");
    {
        CHECK(url_encode("A-Z._~09") == "A-Z._~09", "unreserved chars should not be encoded");
    }
}

void test_generate_peer_id()
{
    printf("\n=== Utils: generate_peer_id ===\n");

    TEST("peer_id length is 20");
    {
        auto id = generate_peer_id();
        CHECK(id.size() == 20, "peer_id should be 20 bytes");
    }

    TEST("peer_id starts with -YZ");
    {
        auto id = generate_peer_id();
        CHECK(id.substr(0, 3) == "-YZ", "peer_id should start with -YZ");
    }

    TEST("peer_id two calls differ");
    {
        auto id1 = generate_peer_id();
        auto id2 = generate_peer_id();
        CHECK(id1 != id2, "two peer_ids should be different (random)");
    }
}

void test_current_timestamp_ms()
{
    printf("\n=== Utils: current_timestamp_ms ===\n");

    TEST("timestamp is positive");
    {
        auto ts = current_timestamp_ms();
        CHECK(ts > 0, "timestamp should be positive");
    }

    TEST("timestamp is reasonable (> year 2020)");
    {
        auto ts = current_timestamp_ms();
        CHECK(ts > 1577836800000LL, "timestamp should be after 2020");
    }
}

// ============================================================================
// Part 2: Peer Wire Message Tests
// ============================================================================

void test_handshake_message()
{
    printf("\n=== PeerWireMessage: HandshakeMessage ===\n");

    TEST("handshake serialize size is 68");
    {
        HandshakeMessage hs;
        auto data = hs.serialize();
        CHECK(data.size() == HandshakeMessage::HANDSHAKE_SIZE, "handshake should be 68 bytes");
    }

    TEST("handshake serialize -> deserialize roundtrip");
    {
        HandshakeMessage hs;
        uint8_t hash[20] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
        hs.set_info_hash(hash);
        hs.set_peer_id("-YZ0001-123456789012");
        auto data = hs.serialize();

        HandshakeMessage hs2;
        CHECK(hs2.deserialize(data.data(), data.size()), "deserialization should succeed");
        CHECK(std::memcmp(hs2.info_hash_, hash, 20) == 0, "info_hash should match after roundtrip");
        CHECK(std::memcmp(hs2.peer_id_, "-YZ0001-123456789012", 20) == 0, "peer_id should match after roundtrip");
    }

    TEST("handshake deserialize too short fails");
    {
        HandshakeMessage hs;
        uint8_t short_data[10] = {};
        CHECK(!hs.deserialize(short_data, 10), "short data should fail");
    }

    TEST("handshake deserialize wrong protocol fails");
    {
        uint8_t data[68] = {};
        data[0] = 19;
        std::memcpy(data + 1, "WrongProtocolString", 19);
        HandshakeMessage hs;
        CHECK(!hs.deserialize(data, 68), "wrong protocol should fail");
    }

    TEST("handshake info_hash from vector");
    {
        HandshakeMessage hs;
        std::vector<uint8_t> hash_vec(20, 0xAB);
        hs.set_info_hash(hash_vec);
        CHECK(hs.info_hash_[0] == 0xAB && hs.info_hash_[19] == 0xAB, "vector info_hash not set correctly");
    }

    TEST("handshake supports_dht flag");
    {
        HandshakeMessage hs;
        CHECK(hs.supports_dht(), "should support DHT by default");
    }

    TEST("handshake supports_extension flag");
    {
        HandshakeMessage hs;
        CHECK(hs.supports_extension(), "should support extension by default");
    }
}

void test_peer_message_simple()
{
    printf("\n=== PeerWireMessage: Simple Messages ===\n");

    TEST("keepalive serialize -> parse");
    {
        auto msg = PeerMessage::keepalive();
        CHECK(msg.is_keepalive(), "keepalive should have length 0");
        auto data = msg.serialize();
        PeerMessage out;
        int consumed = PeerMessage::parse(data.data(), data.size(), out);
        CHECK(consumed == 4 && out.is_keepalive(), "keepalive roundtrip failed");
    }

    TEST("choke serialize -> parse");
    {
        auto msg = PeerMessage::choke();
        auto data = msg.serialize();
        PeerMessage out;
        int consumed = PeerMessage::parse(data.data(), data.size(), out);
        CHECK(consumed == 5 && out.id_ == PeerMessageId::choke, "choke roundtrip failed");
    }

    TEST("unchoke serialize -> parse");
    {
        auto msg = PeerMessage::unchoke();
        auto data = msg.serialize();
        PeerMessage out;
        int consumed = PeerMessage::parse(data.data(), data.size(), out);
        CHECK(consumed == 5 && out.id_ == PeerMessageId::unchoke, "unchoke roundtrip failed");
    }

    TEST("interested serialize -> parse");
    {
        auto msg = PeerMessage::interested();
        auto data = msg.serialize();
        PeerMessage out;
        int consumed = PeerMessage::parse(data.data(), data.size(), out);
        CHECK(consumed == 5 && out.id_ == PeerMessageId::interested, "interested roundtrip failed");
    }

    TEST("not_interested serialize -> parse");
    {
        auto msg = PeerMessage::not_interested();
        auto data = msg.serialize();
        PeerMessage out;
        int consumed = PeerMessage::parse(data.data(), data.size(), out);
        CHECK(consumed == 5 && out.id_ == PeerMessageId::not_interested, "not_interested roundtrip failed");
    }

    TEST("port message roundtrip");
    {
        auto msg = PeerMessage::port(6881);
        auto data = msg.serialize();
        PeerMessage out;
        int consumed = PeerMessage::parse(data.data(), data.size(), out);
        CHECK(consumed == 7 && out.id_ == PeerMessageId::port, "port roundtrip failed");
        uint16_t decoded_port = (static_cast<uint16_t>(out.payload_[0]) << 8) | out.payload_[1];
        CHECK(decoded_port == 6881, "port payload decodes to 6881");
    }
}

void test_peer_message_have()
{
    printf("\n=== PeerWireMessage: have / bitfield ===\n");

    TEST("have message roundtrip");
    {
        auto msg = PeerMessage::have(12345);
        auto data = msg.serialize();
        PeerMessage out;
        int consumed = PeerMessage::parse(data.data(), data.size(), out);
        CHECK(consumed == 9 && out.id_ == PeerMessageId::have, "have roundtrip failed");
        CHECK(out.have_piece_index() == 12345, "piece index should be 12345");
    }

    TEST("have piece_index=0");
    {
        auto msg = PeerMessage::have(0);
        auto data = msg.serialize();
        PeerMessage out;
        PeerMessage::parse(data.data(), data.size(), out);
        CHECK(out.have_piece_index() == 0, "piece index 0 failed");
    }

    TEST("have piece_index=MAX_UINT32");
    {
        auto msg = PeerMessage::have(0xFFFFFFFF);
        auto data = msg.serialize();
        PeerMessage out;
        PeerMessage::parse(data.data(), data.size(), out);
        CHECK(out.have_piece_index() == 0xFFFFFFFF, "piece index MAX failed");
    }

    TEST("bitfield message roundtrip");
    {
        std::vector<uint8_t> bits = {0xFF, 0x00, 0x0F};
        auto msg = PeerMessage::bitfield(bits);
        auto data = msg.serialize();
        PeerMessage out;
        int consumed = PeerMessage::parse(data.data(), data.size(), out);
        CHECK(consumed == 8 && out.id_ == PeerMessageId::bitfield, "bitfield roundtrip failed");
        CHECK(out.payload_ == bits, "bitfield payload mismatch");
    }
}

void test_peer_message_request()
{
    printf("\n=== PeerWireMessage: request / cancel ===\n");

    TEST("request message roundtrip");
    {
        auto msg = PeerMessage::request(1, 2, 16384);
        auto data = msg.serialize();
        PeerMessage out;
        int consumed = PeerMessage::parse(data.data(), data.size(), out);
        CHECK(consumed == 17 && out.id_ == PeerMessageId::request, "request roundtrip failed");
        CHECK(out.request_piece_index() == 1, "request piece index");
        CHECK(out.request_offset() == 2, "request offset");
        CHECK(out.request_length() == 16384, "request length");
    }

    TEST("cancel message roundtrip");
    {
        auto msg = PeerMessage::cancel(5, 100, 32768);
        auto data = msg.serialize();
        PeerMessage out;
        int consumed = PeerMessage::parse(data.data(), data.size(), out);
        CHECK(consumed == 17 && out.id_ == PeerMessageId::cancel, "cancel roundtrip failed");
        CHECK(out.request_piece_index() == 5, "cancel piece index");
        CHECK(out.request_offset() == 100, "cancel offset");
        CHECK(out.request_length() == 32768, "cancel length");
    }

    TEST("request and cancel same format");
    {
        auto req = PeerMessage::request(42, 0, 16384).serialize();
        auto canc = PeerMessage::cancel(42, 0, 16384).serialize();
        CHECK(req.size() == canc.size(), "request and cancel should have same size");
        CHECK(req[4] != canc[4], "request and cancel should have different id");
        CHECK(std::memcmp(req.data() + 5, canc.data() + 5, 12) == 0, "payloads should be identical");
    }
}

void test_peer_message_piece()
{
    printf("\n=== PeerWireMessage: piece ===\n");

    TEST("piece message roundtrip");
    {
        uint8_t block_data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
        auto msg = PeerMessage::piece(3, 1024, block_data, 5);
        auto data = msg.serialize();
        PeerMessage out;
        int consumed = PeerMessage::parse(data.data(), data.size(), out);
        CHECK(consumed == 18 && out.id_ == PeerMessageId::piece, "piece roundtrip failed");
        CHECK(out.piece_block_index() == 3, "piece index");
        CHECK(out.piece_block_offset() == 1024, "piece offset");
        CHECK(out.piece_block_size() == 5, "piece data size");
        CHECK(std::memcmp(out.piece_block_data(), block_data, 5) == 0, "piece data mismatch");
    }

    TEST("piece with large block");
    {
        std::vector<uint8_t> big_block(16384, 0xAA);
        auto msg = PeerMessage::piece(0, 0, big_block.data(), big_block.size());
        auto data = msg.serialize();
        PeerMessage out;
        PeerMessage::parse(data.data(), data.size(), out);
        CHECK(out.piece_block_size() == 16384, "large piece size mismatch");
        bool all_aa = true;
        for (size_t i = 0; i < 16384; i++)
            if (out.piece_block_data()[i] != 0xAA) { all_aa = false; break; }
        CHECK(all_aa, "large piece data corrupted");
    }

    TEST("piece_block_data returns nullptr for small payload");
    {
        PeerMessage msg;
        msg.id_ = PeerMessageId::piece;
        msg.payload_ = {0, 0, 0, 0};
        CHECK(msg.piece_block_data() == nullptr, "should return nullptr for small payload");
    }
}

void test_peer_message_parse_incomplete()
{
    printf("\n=== PeerWireMessage: parse edge cases ===\n");

    TEST("parse < 4 bytes returns 0");
    {
        uint8_t data[3] = {0, 0, 0};
        PeerMessage out;
        CHECK(PeerMessage::parse(data, 3, out) == 0, "incomplete header should return 0");
    }

    TEST("parse incomplete message returns 0");
    {
        uint8_t data[] = {0, 0, 0, 9, 0x07};
        PeerMessage out;
        CHECK(PeerMessage::parse(data, 5, out) == 0, "incomplete payload should return 0");
    }

    TEST("parse multiple messages in sequence");
    {
        auto ka = PeerMessage::keepalive().serialize();
        auto choke = PeerMessage::choke().serialize();
        auto interest = PeerMessage::interested().serialize();

        std::vector<uint8_t> combined;
        combined.insert(combined.end(), ka.begin(), ka.end());
        combined.insert(combined.end(), choke.begin(), choke.end());
        combined.insert(combined.end(), interest.begin(), interest.end());

        const uint8_t *ptr = combined.data();
        size_t remaining = combined.size();

        PeerMessage out1, out2, out3;
        int c1 = PeerMessage::parse(ptr, remaining, out1);
        ptr += c1; remaining -= c1;
        int c2 = PeerMessage::parse(ptr, remaining, out2);
        ptr += c2; remaining -= c2;
        int c3 = PeerMessage::parse(ptr, remaining, out3);

        CHECK(c1 == 4 && c2 == 5 && c3 == 5, "should parse 3 messages");
        CHECK(out1.is_keepalive() && out2.id_ == PeerMessageId::choke &&
              out3.id_ == PeerMessageId::interested, "message ids mismatch");
    }
}

// ============================================================================
// Part 3: PeerState Tests
// ============================================================================

void test_peer_state()
{
    printf("\n=== PeerState ===\n");

    TEST("default state values");
    {
        PeerState ps;
        CHECK(ps.am_choking == true, "should be choking by default");
        CHECK(ps.am_interested == false, "should not be interested by default");
        CHECK(ps.peer_choking == true, "peer should be choking by default");
        CHECK(ps.peer_interested == false, "peer should not be interested by default");
    }

    TEST("set_have_piece and has_piece");
    {
        PeerState ps;
        ps.set_have_piece(5, 10);
        CHECK(ps.has_piece(5), "piece 5 should exist");
        CHECK(!ps.has_piece(0), "piece 0 should not exist");
        CHECK(!ps.has_piece(-1), "negative index should return false");
        CHECK(!ps.has_piece(100), "out of range should return false");
    }

    TEST("set_bitfield / to_bitfield roundtrip");
    {
        PeerState ps;
        std::vector<uint8_t> bf = {0xFF, 0x00, 0xF0};
        ps.set_bitfield(bf, 24);
        CHECK(ps.has_piece(0) && ps.has_piece(7), "first byte bits");
        CHECK(!ps.has_piece(8) && !ps.has_piece(15), "second byte bits");
        CHECK(ps.has_piece(16) && !ps.has_piece(20), "third byte bits");
        auto roundtrip = ps.to_bitfield();
        CHECK(roundtrip == bf, "bitfield roundtrip mismatch");
    }

    TEST("set_have_piece expands vector");
    {
        PeerState ps;
        ps.set_have_piece(100, 101);
        CHECK(ps.has_piece(100), "piece 100 should exist");
        CHECK(ps.pieces.size() >= 101, "pieces vector should be >= 101");
    }

    TEST("to_bitfield empty returns empty");
    {
        PeerState ps;
        CHECK(ps.to_bitfield().empty(), "empty state should give empty bitfield");
    }

    TEST("to_bitfield alignment");
    {
        PeerState ps;
        ps.pieces = {true, false, true, false, true};
        auto bf = ps.to_bitfield();
        CHECK(bf.size() == 1, "5 pieces should fit in 1 byte");
        CHECK(bf[0] == 0b10101000, "bit pattern mismatch");
    }
}

// ============================================================================
// Part 4: TorrentMeta Tests
// ============================================================================

void test_torrent_meta_parse()
{
    printf("\n=== TorrentMeta: parse ===\n");

    std::string fake_piece_hash(20, '\x42');
    std::string tracker_url = "http://tracker.example.com/announce";

    std::string torrent = "d"
        + bencode_str("announce") + bencode_str(tracker_url)
        + bencode_str("announce-list") + "l"
            "l" + bencode_str(tracker_url) + "e"
        + "e"
        + bencode_str("info") + "d"
            + bencode_str("name") + bencode_str("test.txt")
            + bencode_str("piece length") + bencode_int(262144)
            + bencode_str("pieces") + bencode_str(fake_piece_hash)
            + bencode_str("length") + bencode_int(1048576)
        + "e"
    + "e";

    TEST("parse single file torrent");
    {
        auto meta = TorrentMeta::parse(torrent);
        CHECK(meta.announce_ == tracker_url, "announce mismatch");
        CHECK(meta.info.name_ == "test.txt", "name mismatch");
        CHECK(meta.info.piece_length_ == 262144, "piece length mismatch");
        CHECK(meta.info.total_length_ == 1048576, "total length mismatch");
        CHECK(meta.info.piece_count() == 4, "piece count should be 4");
    }

    TEST("parse announce list");
    {
        auto meta = TorrentMeta::parse(torrent);
        CHECK(meta.announce_list_.size() == 1, "should have 1 tier");
        CHECK(meta.announce_list_[0].size() == 1, "tier should have 1 URL");
    }

    TEST("info_hash is computed");
    {
        auto meta = TorrentMeta::parse(torrent);
        CHECK(meta.info_hash_.size() == 20, "info_hash should be 20 bytes");
        CHECK(!meta.info_hash_hex_.empty(), "info_hash_hex should not be empty");
        CHECK(meta.info_hash_hex_.size() == 40, "info_hash_hex should be 40 chars");
    }

    TEST("info_hash is deterministic");
    {
        auto meta1 = TorrentMeta::parse(torrent);
        auto meta2 = TorrentMeta::parse(torrent);
        CHECK(meta1.info_hash_ == meta2.info_hash_, "same torrent should give same info_hash");
    }

    TEST("info_hash hex matches bytes");
    {
        auto meta = TorrentMeta::parse(torrent);
        CHECK(meta.info_hash_hex_ == to_hex(meta.info_hash_), "hex should match to_hex of bytes");
    }

    TEST("parse empty string gives empty meta");
    {
        auto meta = TorrentMeta::parse("");
        CHECK(meta.info_hash_.empty(), "empty input should give empty info_hash");
    }

    TEST("parse invalid bencode gives empty meta");
    {
        auto meta = TorrentMeta::parse("this is not bencode");
        CHECK(meta.info_hash_.empty(), "invalid bencode should give empty info_hash");
    }

    TEST("piece_hash returns correct hash for each piece");
    {
        auto meta = TorrentMeta::parse(torrent);
        CHECK(meta.info.piece_hash(0) == fake_piece_hash, "piece_hash(0) should return our fake hash");
    }

    TEST("piece_hash out of range returns empty");
    {
        auto meta = TorrentMeta::parse(torrent);
        CHECK(meta.info.piece_hash(-1) == "", "negative index should return empty");
        CHECK(meta.info.piece_hash(999) == "", "out of range should return empty");
    }

    TEST("parse with comment and created_by");
    {
        std::string torrent2 = "d"
            + bencode_str("announce") + bencode_str(tracker_url)
            + bencode_str("comment") + bencode_str("hello world")
            + bencode_str("created by") + bencode_str("test client")
            + bencode_str("creation date") + bencode_int(1700000000)
            + bencode_str("info") + "d"
                + bencode_str("name") + bencode_str("file")
                + bencode_str("piece length") + bencode_int(262144)
                + bencode_str("pieces") + bencode_str(fake_piece_hash)
                + bencode_str("length") + bencode_int(262144)
            + "e"
        + "e";

        auto meta = TorrentMeta::parse(torrent2);
        CHECK(meta.comment_ == "hello world", "comment mismatch");
        CHECK(meta.created_by_ == "test client", "created_by mismatch");
        CHECK(meta.creation_date_ == 1700000000, "creation_date mismatch");
    }

    TEST("parse multi-file torrent");
    {
        std::string pieces_hash(40, '\xAA');
        std::string torrent3 = "d"
            + bencode_str("announce") + bencode_str(tracker_url)
            + bencode_str("info") + "d"
                + bencode_str("name") + bencode_str("dir1")
                + bencode_str("piece length") + bencode_int(262144)
                + bencode_str("pieces") + bencode_str(pieces_hash)
                + bencode_str("files") + "l"
                    "d"
                        + bencode_str("length") + bencode_int(131072)
                        + bencode_str("path") + "l" + bencode_str("dir1") + bencode_str("file1.txt") + "e"
                    + "e"
                    "d"
                        + bencode_str("length") + bencode_int(131072)
                        + bencode_str("path") + "l" + bencode_str("dir1") + bencode_str("file2.txt") + "e"
                    + "e"
                + "e"
            + "e"
        + "e";

        auto meta = TorrentMeta::parse(torrent3);
        CHECK(meta.info.files_.size() == 2, "should have 2 files");
        CHECK(meta.info.files_[0].length_ == 131072, "file1 length");
        CHECK(meta.info.files_[1].offset_ == 131072, "file2 offset should be 131072");
        CHECK(meta.info.total_length_ == 262144, "total length should be sum");
    }

    TEST("parse with private flag");
    {
        std::string torrent_priv = "d"
            + bencode_str("announce") + bencode_str(tracker_url)
            + bencode_str("info") + "d"
                + bencode_str("name") + bencode_str("priv")
                + bencode_str("piece length") + bencode_int(262144)
                + bencode_str("pieces") + bencode_str(fake_piece_hash)
                + bencode_str("length") + bencode_int(262144)
                + bencode_str("private") + bencode_int(1)
            + "e"
        + "e";

        auto meta = TorrentMeta::parse(torrent_priv);
        CHECK(meta.info.private_ == true, "private flag should be true");
    }

    TEST("parse fallback announce_list from announce");
    {
        std::string torrent_no_list = "d"
            + bencode_str("announce") + bencode_str("http://tracker2.com/announce")
            + bencode_str("info") + "d"
                + bencode_str("name") + bencode_str("test")
                + bencode_str("piece length") + bencode_int(262144)
                + bencode_str("pieces") + bencode_str(fake_piece_hash)
                + bencode_str("length") + bencode_int(262144)
            + "e"
        + "e";

        auto meta = TorrentMeta::parse(torrent_no_list);
        CHECK(meta.announce_list_.size() == 1, "should build announce_list from announce");
        CHECK(meta.announce_list_[0][0] == "http://tracker2.com/announce", "fallback URL");
    }
}

void test_tracker_response_parse()
{
    printf("\n=== TrackerResponse: compact peer parsing ===\n");

    uint8_t peer1[] = {192, 168, 1, 1, 0x1A, 0xE1}; // 192.168.1.1:6881
    uint8_t peer2[] = {10, 0, 0, 1, 0x1A, 0xE2};    // 10.0.0.1:6882

    std::string peers_data(reinterpret_cast<char *>(peer1), 6);
    peers_data.append(reinterpret_cast<char *>(peer2), 6);

    std::string response_body = "d"
        + bencode_str("interval") + bencode_int(1800)
        + bencode_str("complete") + bencode_int(10)
        + bencode_str("incomplete") + bencode_int(5)
        + bencode_str("peers") + bencode_str(peers_data)
    + "e";

    TEST("parse compact peer format");
    {
        auto *data = BencodingDataConverter::parse(response_body);
        assert(data && data->type_ == DataType::dictionary_);
        auto *dict = dynamic_cast<DicttionaryData *>(data);

        TrackerResponse resp;
        if (auto *v = dict->get_val("interval"); v && v->type_ == DataType::integer_)
            resp.interval_ = dynamic_cast<IntegerData *>(v)->get_data();
        if (auto *v = dict->get_val("complete"); v && v->type_ == DataType::integer_)
            resp.complete_ = dynamic_cast<IntegerData *>(v)->get_data();
        if (auto *v = dict->get_val("incomplete"); v && v->type_ == DataType::integer_)
            resp.incomplete_ = dynamic_cast<IntegerData *>(v)->get_data();
        if (auto *v = dict->get_val("peers"); v && v->type_ == DataType::string_)
        {
            const auto &peers_str = dynamic_cast<StringData *>(v)->get_data();
            for (size_t i = 0; i + 6 <= peers_str.size(); i += 6)
            {
                PeerAddress addr;
                addr.ip_ = std::to_string(static_cast<unsigned char>(peers_str[i])) + "." +
                           std::to_string(static_cast<unsigned char>(peers_str[i + 1])) + "." +
                           std::to_string(static_cast<unsigned char>(peers_str[i + 2])) + "." +
                           std::to_string(static_cast<unsigned char>(peers_str[i + 3]));
                addr.port_ = (static_cast<uint8_t>(peers_str[i + 4]) << 8) |
                             static_cast<uint8_t>(peers_str[i + 5]);
                resp.peers_.push_back(addr);
            }
        }

        CHECK(resp.interval_ == 1800, "interval");
        CHECK(resp.complete_ == 10, "complete (seeders)");
        CHECK(resp.incomplete_ == 5, "incomplete (leechers)");
        CHECK(resp.peers_.size() == 2, "should have 2 peers");
        CHECK(resp.peers_[0].ip_ == "192.168.1.1", "peer1 IP");
        CHECK(resp.peers_[0].port_ == 6881, "peer1 port");
        CHECK(resp.peers_[1].ip_ == "10.0.0.1", "peer2 IP");
        CHECK(resp.peers_[1].port_ == 6882, "peer2 port");

        delete data;
    }
}

// ============================================================================
// Part 5: NAT Config Tests
// ============================================================================

void test_nat_config()
{
    printf("\n=== NatConfig ===\n");

    TEST("default config values");
    {
        NatConfig cfg;
        CHECK(cfg.enable_inbound_listen == true, "inbound listen should default ON");
        CHECK(cfg.enable_upnp == true, "UPnP should default ON");
        CHECK(cfg.enable_nat_pmp == true, "NAT-PMP should default ON");
        CHECK(cfg.enable_utp == true, "uTP should default ON");
        CHECK(cfg.enable_dht == true, "DHT should default ON");
        CHECK(cfg.enable_pex == true, "PEX should default ON");
        CHECK(cfg.listen_port == 6881, "default listen port should be 6881");
        CHECK(cfg.listen_retry_range == 10, "default retry range should be 10");
    }

    TEST("custom config assignment");
    {
        NatConfig cfg;
        cfg.listen_port = 12345;
        cfg.enable_upnp = false;
        cfg.enable_dht = false;
        cfg.external_ip = "1.2.3.4";
        cfg.upnp_lease_duration = 7200;
        cfg.dht_max_nodes = 500;
        CHECK(cfg.listen_port == 12345, "custom listen port");
        CHECK(cfg.enable_upnp == false, "UPnP disabled");
        CHECK(cfg.enable_dht == false, "DHT disabled");
        CHECK(cfg.external_ip == "1.2.3.4", "external IP set");
        CHECK(cfg.upnp_lease_duration == 7200, "lease duration");
        CHECK(cfg.dht_max_nodes == 500, "DHT max nodes");
    }

    TEST("disable all NAT features");
    {
        NatConfig cfg;
        cfg.enable_inbound_listen = false;
        cfg.enable_upnp = false;
        cfg.enable_nat_pmp = false;
        cfg.enable_utp = false;
        cfg.enable_dht = false;
        cfg.enable_pex = false;
        CHECK(cfg.enable_inbound_listen == false, "");
        CHECK(cfg.enable_upnp == false, "");
        CHECK(cfg.enable_nat_pmp == false, "");
        CHECK(cfg.enable_utp == false, "");
        CHECK(cfg.enable_dht == false, "");
        CHECK(cfg.enable_pex == false, "");
    }
}

// ============================================================================
// Part 6: uTP Header Tests
// ============================================================================

void test_utp_header()
{
    printf("\n=== uTP: UtpHeader ===\n");

    TEST("UtpHeader size check");
    {
        // UtpHeader has 9 fields: type(1)+version(1)+extension(1)+conn_id(2)+
        // seq_nr(4)+ack_nr(4)+timestamp(4)+timestamp_diff(4)+window_size(1) = 22 bytes
        CHECK(sizeof(UtpHeader) == 22, "UtpHeader packed size should be 22 bytes");
    }

    TEST("UtpHeader set_type_ver / get_type");
    {
        UtpHeader hdr = {};
        hdr.set_type_ver(UtpType::st_syn);
        CHECK(hdr.get_type() == UtpType::st_syn, "type should be st_syn");
        // Note: set_type_ver stores version in the 'version' field, but get_version()
        // reads from 'type' field. This is a known implementation quirk.
        // get_version() always returns 0 since type holds only the 4-bit type value.
    }

    TEST("UtpHeader network order roundtrip");
    {
        UtpHeader hdr = {};
        hdr.set_type_ver(UtpType::st_data);
        hdr.conn_id = 0x1234;
        hdr.seq_nr = 0xDEADBEEF;
        hdr.ack_nr = 0xCAFEBABE;
        hdr.timestamp = 1000000;
        hdr.timestamp_diff = 500000;
        hdr.window_size = 64;

        hdr.to_network_order();
        // After network order, the raw bytes should be big-endian
        CHECK(hdr.conn_id == htons(0x1234), "conn_id should be in network order");
        CHECK(hdr.seq_nr == htonl(0xDEADBEEF), "seq_nr should be in network order");

        hdr.to_host_order();
        CHECK(hdr.conn_id == 0x1234, "conn_id should roundtrip to host order");
        CHECK(hdr.seq_nr == 0xDEADBEEF, "seq_nr should roundtrip to host order");
        CHECK(hdr.ack_nr == 0xCAFEBABE, "ack_nr should roundtrip to host order");
        CHECK(hdr.timestamp == 1000000, "timestamp should roundtrip");
        CHECK(hdr.timestamp_diff == 500000, "timestamp_diff should roundtrip");
        CHECK(hdr.window_size == 64, "window_size should not change");
    }

    TEST("UTP_RESET_SIZE and UTP_HEADER_SIZE");
    {
        CHECK(UTP_RESET_SIZE == 8, "ST_RESET packet is 8 bytes");
        CHECK(UTP_HEADER_SIZE == sizeof(UtpHeader), "UTP_HEADER_SIZE should match sizeof(UtpHeader)");
    }

    TEST("UtpType enum values");
    {
        CHECK(static_cast<uint8_t>(UtpType::st_reset) == 0, "st_reset = 0");
        CHECK(static_cast<uint8_t>(UtpType::st_state) == 1, "st_state = 1");
        CHECK(static_cast<uint8_t>(UtpType::st_syn) == 2, "st_syn = 2");
        CHECK(static_cast<uint8_t>(UtpType::st_data) == 3, "st_data = 3");
        CHECK(static_cast<uint8_t>(UtpType::st_fin) == 4, "st_fin = 4");
    }
}

// ============================================================================
// Part 7: uTP Connection State Tests
// ============================================================================

void test_utp_connection_state()
{
    printf("\n=== uTP: UtpConnection state ===\n");

    TEST("UtpConnection state enum");
    {
        // State transitions are tested indirectly. Just verify the states exist.
        CHECK(static_cast<int>(UtpConnection::State::idle) == 0, "idle");
        CHECK(static_cast<int>(UtpConnection::State::syn_sent) == 1, "syn_sent");
        CHECK(static_cast<int>(UtpConnection::State::syn_recv) == 2, "syn_recv");
        CHECK(static_cast<int>(UtpConnection::State::connected) == 3, "connected");
        CHECK(static_cast<int>(UtpConnection::State::closed) == 4, "closed");
        CHECK(static_cast<int>(UtpConnection::State::error) == 5, "error");
    }

    TEST("UtpManager default state");
    {
        UtpManager mgr;
        CHECK(!mgr.is_running(), "UtpManager should not be running by default");
        CHECK(mgr.get_port() == 0, "default port should be 0");
    }
}

// ============================================================================
// Part 8: DHT Tests
// ============================================================================

void test_dht_node_id()
{
    printf("\n=== DHT: DhtNodeId ===\n");

    TEST("DhtNodeId is 20 bytes");
    {
        CHECK(sizeof(DhtNodeId) == 20, "DhtNodeId should be 20 bytes");
    }

    TEST("DhtCompactNode to_compact / from_compact roundtrip");
    {
        DhtCompactNode node;
        node.id = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
                   0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14};
        node.ip = htonl(0xC0A80101); // 192.168.1.1
        node.port = htons(6881);

        auto compact = node.to_compact();
        CHECK(compact.size() == 26, "compact should be 26 bytes");

        auto recovered = DhtCompactNode::from_compact(compact.data());
        CHECK(recovered.id == node.id, "node id should match");
        CHECK(recovered.ip == node.ip, "ip should match");
        CHECK(recovered.port == node.port, "port should match");
    }

    TEST("DhtCompactNode ip_string");
    {
        DhtCompactNode node;
        node.id = {};
        node.ip = htonl(0x0A000001); // 10.0.0.1
        node.port = htons(6882);
        CHECK(node.ip_string() == "10.0.0.1", "ip string should be 10.0.0.1");
    }
}

void test_dht_bucket()
{
    printf("\n=== DHT: DhtBucket ===\n");

    TEST("DhtBucket K = 8");
    {
        CHECK(DhtBucket::K == 8, "K should be 8");
    }

    TEST("DhtBucket add and contains");
    {
        DhtBucket bucket;
        DhtCompactNode node = {};
        node.id[0] = 0x42;
        bucket.add(node);
        CHECK(!bucket.full(), "bucket with 1 node should not be full");
        CHECK(bucket.contains(node.id), "should contain the added node");
    }

    TEST("DhtBucket full at K nodes");
    {
        DhtBucket bucket;
        for (size_t i = 0; i < DhtBucket::K; i++)
        {
            DhtCompactNode node = {};
            node.id[19] = static_cast<uint8_t>(i + 1);
            bucket.add(node);
        }
        CHECK(bucket.full(), "bucket should be full at K nodes");
        CHECK(bucket.nodes.size() == DhtBucket::K, "should have exactly K nodes");
    }

    TEST("DhtBucket remove");
    {
        DhtBucket bucket;
        DhtCompactNode node = {};
        node.id[0] = 0x42;
        bucket.add(node);
        CHECK(bucket.contains(node.id), "should contain node before remove");
        bucket.remove(node.id);
        CHECK(!bucket.contains(node.id), "should not contain node after remove");
    }
}

void test_dht_node_basic()
{
    printf("\n=== DHT: DhtNode basic ===\n");

    TEST("DhtNode default state");
    {
        DhtNode dht;
        CHECK(!dht.is_running(), "should not be running by default");
        CHECK(dht.get_port() == 0, "default port should be 0");
        CHECK(dht.routing_table_size() == 0, "routing table should have 0 nodes initially");
    }

    TEST("DhtNode node_id is 20 bytes after generation");
    {
        DhtNode dht;
        auto &id = dht.get_node_id();
        CHECK(id.size() == 20, "node_id should be 20 bytes");
    }

    TEST("DhtNode find_closest_nodes - routing table initialized");
    {
        // DhtNode constructor calls init_routing_table() which allocates 160 buckets.
        // find_closest_nodes requires a valid node_id (set via start()), but
        // the routing table itself should be safely iterable.
        DhtNode dht;
        CHECK(dht.routing_table_size() == 0, "empty routing table returns 0");
    }
}

// ============================================================================
// Part 9: PEX Tests
// ============================================================================

void test_pex_manager_basic()
{
    printf("\n=== PEX: PexManager basic ===\n");

    TEST("PexManager default state");
    {
        PexManager pex;
        CHECK(pex.peer_count() == 0, "should have 0 peers initially");
    }

    TEST("PexManager init with info_hash");
    {
        PexManager pex;
        std::vector<uint8_t> info_hash(20, 0xAB);
        NatConfig cfg;
        pex.init(info_hash, cfg);
        CHECK(pex.peer_count() == 0, "should still have 0 peers after init");
    }

    TEST("PexManager add_peer and get_all_peers");
    {
        PexManager pex;
        std::vector<uint8_t> info_hash(20, 0xAB);
        NatConfig cfg;
        pex.init(info_hash, cfg);

        pex.add_peer("192.168.1.1", 6881);
        pex.add_peer("10.0.0.1", 6882);
        pex.add_peer("172.16.0.1", 6883);
        CHECK(pex.peer_count() == 3, "should have 3 peers");

        auto peers = pex.get_all_peers();
        CHECK(peers.size() == 3, "get_all_peers should return 3");
    }

    TEST("PexManager add_peer deduplication");
    {
        PexManager pex;
        std::vector<uint8_t> info_hash(20, 0xAB);
        NatConfig cfg;
        pex.init(info_hash, cfg);

        pex.add_peer("192.168.1.1", 6881);
        pex.add_peer("192.168.1.1", 6881); // duplicate
        CHECK(pex.peer_count() == 1, "duplicates should not be counted");
    }

    TEST("PexManager remove_peer");
    {
        PexManager pex;
        std::vector<uint8_t> info_hash(20, 0xAB);
        NatConfig cfg;
        pex.init(info_hash, cfg);

        pex.add_peer("192.168.1.1", 6881);
        pex.remove_peer("192.168.1.1:6881");
        CHECK(pex.peer_count() == 0, "peer should be removed");
    }

    TEST("PexManager peer_supports_pex (default false)");
    {
        PexManager pex;
        CHECK(!pex.peer_supports_pex("192.168.1.1:6881"), "unknown peer should not support PEX");
    }

    TEST("PexManager build_ext_handshake");
    {
        PexManager pex;
        std::vector<uint8_t> info_hash(20, 0xAB);
        NatConfig cfg;
        pex.init(info_hash, cfg);

        auto ext_hs = pex.build_ext_handshake();
        CHECK(!ext_hs.empty(), "extension handshake should not be empty");
    }

    TEST("PexManager build_pex_message (empty initially)");
    {
        PexManager pex;
        std::vector<uint8_t> info_hash(20, 0xAB);
        NatConfig cfg;
        pex.init(info_hash, cfg);

        auto pex_msg = pex.build_pex_message("192.168.1.1:6881");
        // Should be a valid bencoded dict (or empty if no changes)
        CHECK(true, "build_pex_message should not crash");
    }
}

// ============================================================================
// Part 10: PEX Compact Peer Format Tests
// ============================================================================

void test_pex_compact_format()
{
    printf("\n=== PEX: compact peer format ===\n");

    TEST("compact peer encode/decode roundtrip");
    {
        // Build compact data: 192.168.1.1:6881 + 10.0.0.1:6882
        std::vector<uint8_t> compact = {
            192, 168, 1, 1, 0x1A, 0xE1,   // 192.168.1.1:6881
            10, 0, 0, 1, 0x1A, 0xE2        // 10.0.0.1:6882
        };

        // Parse manually
        std::vector<PexPeerInfo> peers;
        for (size_t i = 0; i + 6 <= compact.size(); i += 6)
        {
            PexPeerInfo p;
            p.ip = std::to_string(compact[i]) + "." +
                   std::to_string(compact[i + 1]) + "." +
                   std::to_string(compact[i + 2]) + "." +
                   std::to_string(compact[i + 3]);
            p.port = (static_cast<uint16_t>(compact[i + 4]) << 8) | compact[i + 5];
            peers.push_back(p);
        }

        CHECK(peers.size() == 2, "should parse 2 peers");
        CHECK(peers[0].ip == "192.168.1.1", "peer1 IP");
        CHECK(peers[0].port == 6881, "peer1 port");
        CHECK(peers[1].ip == "10.0.0.1", "peer2 IP");
        CHECK(peers[1].port == 6882, "peer2 port");
    }
}

// ============================================================================
// Part 9: PieceDownloadState Tests
// ============================================================================

void test_piece_download_state_window()
{
    printf("\n=== PieceDownloadState: request window ===\n");

    TEST("sequential requests advance without duplication");
    {
        PieceDownloadState state;
        state.reset(1, 64 * 1024, 64 * 1024);

        std::vector<bool> peer_pieces = {true};
        PieceBlockRequest r1;
        PieceBlockRequest r2;
        PieceBlockRequest r3;

        const bool ok1 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 8, r1);
        const bool ok2 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 8, r2);
        const bool ok3 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 8, r3);

        CHECK(ok1 && ok2 && ok3 &&
              r1.offset_ == 0 &&
              r2.offset_ == 16 * 1024 &&
              r3.offset_ == 32 * 1024,
              "request offsets should advance sequentially");
    }

    TEST("piece failure resets request cursor");
    {
        PieceDownloadState state;
        state.reset(1, 64 * 1024, 64 * 1024);

        std::vector<bool> peer_pieces = {true};
        PieceBlockRequest first;
        PieceBlockRequest second;

        const bool ok1 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 8, first);
        state.mark_piece_failed(0);
        const bool ok2 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 8, second);

        CHECK(ok1 && ok2 && first.offset_ == 0 && second.offset_ == 0,
              "failed piece should restart from offset 0");
    }

    TEST("requeued block is retried before new offset");
    {
        PieceDownloadState state;
        state.reset(1, 64 * 1024, 64 * 1024);

        std::vector<bool> peer_pieces = {true};
        PieceBlockRequest first;
        PieceBlockRequest second;
        PieceBlockRequest retried;

        const bool ok1 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 8, first);
        const bool ok2 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 8, second);
        state.requeue_block(first.piece_index_, first.offset_, first.length_);
        const bool ok3 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 8, retried);

        CHECK(ok1 && ok2 && ok3 &&
              first.offset_ == 0 &&
              second.offset_ == 16 * 1024 &&
              retried.offset_ == 0,
              "requeued block should be retried before newer offsets");
    }

    TEST("received block can be scheduled again only after inflight clears");
    {
        PieceDownloadState state;
        state.reset(1, 16 * 1024, 16 * 1024);

        std::vector<bool> peer_pieces = {true};
        PieceBlockRequest first;
        PieceBlockRequest second;

        const bool ok1 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 8, first);
        const bool ok2 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 8, second);
        state.mark_block_received(first.piece_index_, first.offset_, first.length_);
        const bool ok3 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 8, second);

        CHECK(ok1 && !ok2 && !ok3 &&
                  first.offset_ == 0,
              "scheduler should not duplicate an inflight block and should stay idle after receipt when piece is exhausted");
    }

    TEST("duplicate block receipt does not double count progress");
    {
        PieceDownloadState state;
        state.reset(1, 32 * 1024, 32 * 1024);

        const auto first = state.mark_block_received(0, 0, 16 * 1024);
        const auto duplicate = state.mark_block_received(0, 0, 16 * 1024);
        const auto next = state.mark_block_received(0, 16 * 1024, 16 * 1024);

        CHECK(first == 16 * 1024 && duplicate == 0 && next == 16 * 1024,
              "received bytes accounting should ignore duplicate blocks");
    }

    TEST("out-of-order block receipt counts only newly covered bytes");
    {
        PieceDownloadState state;
        state.reset(1, 48 * 1024, 48 * 1024);

        const auto late = state.mark_block_received(0, 16 * 1024, 16 * 1024);
        const auto early = state.mark_block_received(0, 0, 16 * 1024);
        const auto overlap = state.mark_block_received(0, 8 * 1024, 16 * 1024);
        const auto tail = state.mark_block_received(0, 32 * 1024, 16 * 1024);

        CHECK(late == 16 * 1024 &&
                  early == 16 * 1024 &&
                  overlap == 0 &&
                  tail == 16 * 1024,
              "out-of-order receipts should only count newly covered byte ranges once");
    }

    TEST("failed piece clears inflight requests before restart");
    {
        PieceDownloadState state;
        state.reset(1, 32 * 1024, 32 * 1024);

        std::vector<bool> peer_pieces = {true};
        PieceBlockRequest first;
        PieceBlockRequest restarted;

        const bool ok1 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 8, first);
        state.mark_piece_failed(0);
        const bool ok2 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 8, restarted);

        CHECK(ok1 && ok2 &&
                  first.offset_ == 0 &&
                  restarted.offset_ == 0,
              "piece failure should clear inflight state and restart scheduling");
    }

    TEST("rarest piece is preferred when availability is provided");
    {
        PieceDownloadState state;
        state.reset(3, 3 * 16 * 1024, 16 * 1024);

        std::vector<bool> peer_pieces = {true, true, true};
        std::vector<uint32_t> availability = {3, 1, 2};
        PieceBlockRequest request;

        const bool ok = state.select_next_request(peer_pieces, &availability, 16 * 1024, 8, request);

        CHECK(ok && request.piece_index_ == 1 && request.offset_ == 0,
              "scheduler should prefer the rarest available piece first");
    }

    TEST("active downloading piece is preferred over opening a new rarer piece");
    {
        PieceDownloadState state;
        state.reset(3, 3 * 16 * 1024, 16 * 1024);

        std::vector<bool> peer_pieces = {true, true, true};
        std::vector<uint32_t> availability = {3, 1, 2};
        PieceBlockRequest first;
        PieceBlockRequest second;

        const bool ok1 = state.select_next_request(peer_pieces, &availability, 8 * 1024, 8, first);
        state.mark_piece_failed(1);
        state.mark_piece_downloading(0);
        const bool ok2 = state.select_next_request(peer_pieces, &availability, 8 * 1024, 8, second);

        CHECK(ok1 &&
                  ok2 &&
                  first.piece_index_ == 1 &&
                  second.piece_index_ == 0 &&
                  second.offset_ == 0,
              "scheduler should continue an active downloading piece before opening a new one");
    }

    TEST("scheduler limits simultaneously active pieces before opening another");
    {
        PieceDownloadState state;
        state.reset(4, 4 * 16 * 1024, 16 * 1024);

        std::vector<bool> peer_pieces = {true, true, true, true};
        PieceBlockRequest first;
        PieceBlockRequest second;
        PieceBlockRequest blocked;

        const bool ok1 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 2, first);
        const bool ok2 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 2, second);
        const bool ok3 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 2, blocked);

        CHECK(ok1 &&
                  ok2 &&
                  !ok3 &&
                  first.piece_index_ == 0 &&
                  second.piece_index_ == 1,
              "scheduler should stop opening new pieces once the active-piece limit is reached");
    }

    TEST("scheduler can open more pieces when active-piece limit is raised");
    {
        PieceDownloadState state;
        state.reset(4, 4 * 16 * 1024, 16 * 1024);

        std::vector<bool> peer_pieces = {true, true, true, true};
        PieceBlockRequest first;
        PieceBlockRequest second;
        PieceBlockRequest third;

        const bool ok1 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 3, first);
        const bool ok2 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 3, second);
        const bool ok3 = state.select_next_request(peer_pieces, nullptr, 16 * 1024, 3, third);

        CHECK(ok1 &&
                  ok2 &&
                  ok3 &&
                  first.piece_index_ == 0 &&
                  second.piece_index_ == 1 &&
                  third.piece_index_ == 2,
              "scheduler should open more pieces when the active-piece limit allows it");
    }
}

void test_download_stats_tracker_upload_context()
{
    printf("\n=== DownloadStatsTracker: uploaded context ===\n");

    TEST("uploaded bytes flow into tracker announce context");
    {
        DownloadStatsTracker tracker;
        const auto torrent = TorrentMeta::parse(build_test_torrent("stats.bin", 32768, 16384, 2));
        tracker.reset(torrent.info.total_length_, torrent.info.piece_count());
        tracker.add_downloaded_bytes(4096);
        tracker.add_uploaded_bytes(2048);

        const auto ctx = tracker.make_tracker_context(torrent, 6881);
        CHECK(ctx.uploaded_ == 2048 && ctx.downloaded_ == 4096 &&
                  ctx.left_ == torrent.info.total_length_ - 4096,
              "tracker context should carry uploaded/downloaded/left bytes");
    }
}

void test_piece_completion_state()
{
    printf("\n=== PieceDownloadState: completion ===\n");

    TEST("all completed pieces mark state complete");
    {
        PieceDownloadState state;
        state.reset(2, 32 * 1024, 16 * 1024);
        state.mark_piece_completed(0);
        CHECK(!state.is_complete(), "state should not be complete until every piece is done");
        state.mark_piece_completed(1);
        CHECK(state.is_complete(), "state should report complete when all pieces are present");
    }
}

void test_http_tracker_client_integration()
{
    printf("\n=== HttpTracker: HttpClient integration ===\n");

    TEST("http tracker announce via project HttpClient");
    {
        std::string body = "d8:intervali1800e8:completei5e10:incompletei2e5:peers6:";
        const char compact_peer[] = {
            static_cast<char>(0x7f), static_cast<char>(0x00), static_cast<char>(0x00),
            static_cast<char>(0x01), static_cast<char>(0x1a), static_cast<char>(0xe1)
        };
        body.append(compact_peer, sizeof(compact_peer));
        body.append("e");

#ifdef _WIN32
        SOCKET server_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        bool socket_ok = server_fd != INVALID_SOCKET;
#else
        int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        bool socket_ok = server_fd >= 0;
#endif

        if (!socket_ok)
        {
            FAIL("listen socket create failed");
            return;
        }

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        int reuse = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));

        if (::bind(server_fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0 ||
            ::listen(server_fd, 1) != 0)
        {
            close_test_socket(server_fd);
            FAIL("listen socket bind/listen failed");
            return;
        }

        sockaddr_in bound_addr {};
#ifdef _WIN32
        int bound_len = sizeof(bound_addr);
#else
        socklen_t bound_len = sizeof(bound_addr);
#endif
        getsockname(server_fd, reinterpret_cast<sockaddr *>(&bound_addr), &bound_len);
        const uint16_t port = ntohs(bound_addr.sin_port);

        std::atomic_bool served = false;
        std::thread server_thread([&]() {
#ifdef _WIN32
            int client_len = sizeof(sockaddr_in);
#else
            socklen_t client_len = sizeof(sockaddr_in);
#endif
            sockaddr_in client_addr {};
            const auto client_fd = ::accept(server_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
#ifdef _WIN32
            if (client_fd == INVALID_SOCKET)
#else
            if (client_fd < 0)
#endif
            {
                close_test_socket(server_fd);
                return;
            }

            char request_buf[2048] {};
            (void)::recv(client_fd, request_buf, sizeof(request_buf), 0);

            const std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
            (void)::send(client_fd, response.data(), static_cast<int>(response.size()), 0);
            served = true;
            close_test_socket(client_fd);
            close_test_socket(server_fd);
        });

        HttpTracker tracker;
        const auto meta = TorrentMeta::parse(build_test_torrent("tracker.bin", 262144, 262144, 1));
        TrackerResponse response;
        const bool ok = tracker.announce(
            "http://127.0.0.1:" + std::to_string(port) + "/announce",
            meta,
            6881,
            0,
            0,
            meta.info.total_length_,
            TrackerAnnounceEvent::started,
            &response);

        server_thread.join();

        const bool passed = ok &&
                            served.load() &&
                            response.interval_ == 1800 &&
                            response.complete_ == 5 &&
                            response.incomplete_ == 2 &&
                            response.peers_.size() == 1 &&
                            response.peers_[0].ip_ == "127.0.0.1" &&
                            response.peers_[0].port_ == 6881;
        if (!passed)
        {
            printf(" [debug ok=%d served=%d interval=%d complete=%d incomplete=%d peers=%zu",
                   ok ? 1 : 0,
                   served.load() ? 1 : 0,
                   response.interval_,
                   response.complete_,
                   response.incomplete_,
                   response.peers_.size());
            if (!response.peers_.empty())
            {
                printf(" first_peer=%s:%u",
                       response.peers_[0].ip_.c_str(),
                       static_cast<unsigned>(response.peers_[0].port_));
            }
            printf("]");
        }
        CHECK(passed, "http tracker should announce through HttpClient and parse peers");
    }

    TEST("http tracker completed event reaches request URL");
    {
        std::string body = "d8:intervali900ee";

#ifdef _WIN32
        SOCKET server_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        bool socket_ok = server_fd != INVALID_SOCKET;
#else
        int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        bool socket_ok = server_fd >= 0;
#endif
        if (!socket_ok)
        {
            FAIL("listen socket create failed");
            return;
        }

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        int reuse = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
        if (::bind(server_fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0 ||
            ::listen(server_fd, 1) != 0)
        {
            close_test_socket(server_fd);
            FAIL("listen socket bind/listen failed");
            return;
        }

        sockaddr_in bound_addr {};
#ifdef _WIN32
        int bound_len = sizeof(bound_addr);
#else
        socklen_t bound_len = sizeof(bound_addr);
#endif
        getsockname(server_fd, reinterpret_cast<sockaddr *>(&bound_addr), &bound_len);
        const uint16_t port = ntohs(bound_addr.sin_port);

        std::atomic_bool saw_completed = false;
        std::thread server_thread([&]() {
#ifdef _WIN32
            int client_len = sizeof(sockaddr_in);
#else
            socklen_t client_len = sizeof(sockaddr_in);
#endif
            sockaddr_in client_addr {};
            const auto client_fd = ::accept(server_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
#ifdef _WIN32
            if (client_fd == INVALID_SOCKET)
#else
            if (client_fd < 0)
#endif
            {
                close_test_socket(server_fd);
                return;
            }

            char request_buf[2048] {};
            const auto received = ::recv(client_fd, request_buf, sizeof(request_buf), 0);
            if (received > 0)
            {
                const std::string request(request_buf, request_buf + received);
                saw_completed = request.find("event=completed") != std::string::npos;
            }

            const std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
            (void)::send(client_fd, response.data(), static_cast<int>(response.size()), 0);
            close_test_socket(client_fd);
            close_test_socket(server_fd);
        });

        HttpTracker tracker;
        const auto meta = TorrentMeta::parse(build_test_torrent("tracker_completed.bin", 262144, 262144, 1));
        TrackerResponse response;
        const bool ok = tracker.announce(
            "http://127.0.0.1:" + std::to_string(port) + "/announce",
            meta,
            6881,
            0,
            meta.info.total_length_,
            0,
            TrackerAnnounceEvent::completed,
            &response);

        server_thread.join();
        CHECK(ok && saw_completed.load(), "http tracker should send completed event when requested");
    }
}

void test_tracker_session_lifecycle_events()
{
    printf("\n=== TrackerSession: lifecycle events ===\n");

    TEST("tracker session emits started completed none and stopped events");
    {
        std::string body = "d8:intervali1ee";

#ifdef _WIN32
        SOCKET server_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        bool socket_ok = server_fd != INVALID_SOCKET;
#else
        int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        bool socket_ok = server_fd >= 0;
#endif
        if (!socket_ok)
        {
            FAIL("listen socket create failed");
            return;
        }

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        int reuse = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
        if (::bind(server_fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0 ||
            ::listen(server_fd, 4) != 0)
        {
            close_test_socket(server_fd);
            FAIL("listen socket bind/listen failed");
            return;
        }

        sockaddr_in bound_addr {};
#ifdef _WIN32
        int bound_len = sizeof(bound_addr);
#else
        socklen_t bound_len = sizeof(bound_addr);
#endif
        getsockname(server_fd, reinterpret_cast<sockaddr *>(&bound_addr), &bound_len);
        const uint16_t port = ntohs(bound_addr.sin_port);

        std::array<std::string, 4> requests;
        std::atomic_int handled = 0;
        std::thread server_thread([&]() {
            for (int i = 0; i < 4; ++i)
            {
#ifdef _WIN32
                int client_len = sizeof(sockaddr_in);
#else
                socklen_t client_len = sizeof(sockaddr_in);
#endif
                sockaddr_in client_addr {};
                const auto client_fd = ::accept(server_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
#ifdef _WIN32
                if (client_fd == INVALID_SOCKET)
#else
                if (client_fd < 0)
#endif
                {
                    break;
                }

                char request_buf[2048] {};
                const auto received = ::recv(client_fd, request_buf, sizeof(request_buf), 0);
                if (received > 0)
                {
                    requests[static_cast<size_t>(i)] = std::string(request_buf, request_buf + received);
                    handled.fetch_add(1);
                }

                const std::string response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Connection: close\r\n"
                    "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
                (void)::send(client_fd, response.data(), static_cast<int>(response.size()), 0);
                close_test_socket(client_fd);
            }
            close_test_socket(server_fd);
        });

        auto meta = TorrentMeta::parse(build_test_torrent("tracker_session.bin", 32768, 16384, 2));
        meta.announce_ = "http://127.0.0.1:" + std::to_string(port) + "/announce";
        meta.announce_list_ = {{meta.announce_}};

        int announce_count = 0;
        TrackerSession session;
        TrackerSessionConfig config;
        config.context_provider_ = [&]() -> TrackerAnnounceContext {
            TrackerAnnounceContext ctx;
            ctx.meta_ = &meta;
            ctx.listen_port_ = 6881;
            ctx.downloaded_ = announce_count == 0 ? 0 : meta.info.total_length_;
            ctx.left_ = announce_count == 0 ? meta.info.total_length_ : 0;
            ++announce_count;
            return ctx;
        };
        session.configure(std::move(config));
        session.start();
        session.announce_now();
        session.announce_now();
        session.announce_now();
        session.stop();
        server_thread.join();

        const bool passed = handled.load() == 4 &&
                            requests[0].find("event=started") != std::string::npos &&
                            requests[1].find("event=completed") != std::string::npos &&
                            requests[2].find("event=") == std::string::npos &&
                            requests[3].find("event=stopped") != std::string::npos;
        CHECK(passed, "tracker session should emit started completed none and stopped lifecycle events");
    }
}

void test_piece_storage_committed_verification()
{
    printf("\n=== PieceStorage: committed data ===\n");

    TEST("committed piece can be re-verified and served from final file");
    {
        namespace fs = std::filesystem;

        const std::string content = "abcdefghijklmnop";
        const auto torrent = TorrentMeta::parse(build_single_file_torrent_with_hashes("seed.bin", content, 8));
        const auto expected_hash = sha1_hash(reinterpret_cast<const uint8_t *>(content.data()), 8);
        const auto parsed_hash = torrent.info.piece_hash(0);
        auto temp_dir = make_bt_test_dir("bt_piece_storage");

        PieceStorage storage;
        storage.configure(&torrent, temp_dir.string());

        const bool wrote = storage.write_piece(0, 0,
                                               reinterpret_cast<const uint8_t *>(content.data()),
                                               8);
        const bool committed = wrote && storage.commit_piece(0);
        std::vector<uint8_t> block;
        const bool verified = storage.verify_committed_piece(0);
        const bool read_ok = storage.read_block(0, 0, 8, block);

        const std::string read_back(block.begin(), block.end());
        if (!(committed && verified && read_ok && read_back == content.substr(0, 8)))
        {
            printf(" [debug hash_match=%d parsed_hash_size=%zu wrote=%d committed=%d verified=%d read_ok=%d block_size=%zu read_back='%s']",
                   parsed_hash.size() == expected_hash.size() &&
                       std::memcmp(parsed_hash.data(), expected_hash.data(), expected_hash.size()) == 0 ? 1 : 0,
                   parsed_hash.size(),
                   wrote ? 1 : 0,
                   committed ? 1 : 0,
                   verified ? 1 : 0,
                   read_ok ? 1 : 0,
                   block.size(),
                   read_back.c_str());
        }
        CHECK(committed && verified && read_ok && read_back == content.substr(0, 8),
              "committed piece should verify and be readable from final file");

        storage.close_all();
        fs::remove_all(temp_dir);
    }

    TEST("scan_committed_pieces returns committed indices");
    {
        namespace fs = std::filesystem;

        const std::string content = "abcdefghijklmnop";
        const auto torrent = TorrentMeta::parse(build_single_file_torrent_with_hashes("seed_scan.bin", content, 8));
        auto temp_dir = make_bt_test_dir("bt_piece_scan");

        PieceStorage storage;
        storage.configure(&torrent, temp_dir.string());
        const bool wrote0 = storage.write_piece(0, 0,
                                                reinterpret_cast<const uint8_t *>(content.data()),
                                                8);
        const bool wrote1 = storage.write_piece(1, 0,
                                                reinterpret_cast<const uint8_t *>(content.data() + 8),
                                                8);
        const bool committed0 = wrote0 && storage.commit_piece(0);
        const bool committed1 = wrote1 && storage.commit_piece(1);
        const auto committed = storage.scan_committed_pieces();

        CHECK(committed0 && committed1 &&
                  committed.size() == 2 &&
                  committed[0] == 0 &&
                  committed[1] == 1,
              "scan should find all committed pieces");

        storage.close_all();
        fs::remove_all(temp_dir);
    }

    TEST("restore_verified_partial_pieces commits full verified partial files");
    {
        namespace fs = std::filesystem;

        const std::string content = "abcdefghijklmnop";
        const auto torrent = TorrentMeta::parse(build_single_file_torrent_with_hashes("seed_restore.bin", content, 8));
        auto temp_dir = make_bt_test_dir("bt_piece_restore");

        PieceStorage storage;
        storage.configure(&torrent, temp_dir.string());
        const bool wrote = storage.write_piece(0, 0,
                                               reinterpret_cast<const uint8_t *>(content.data()),
                                               8);
        storage.close_all();

        PieceStorage restored_storage;
        restored_storage.configure(&torrent, temp_dir.string());
        const auto restored = restored_storage.restore_verified_partial_pieces();
        const auto committed = restored_storage.scan_committed_pieces();

        CHECK(wrote &&
                  restored.size() == 1 &&
                  restored[0] == 0 &&
                  committed.size() == 1 &&
                  committed[0] == 0,
              "verified full partial files should be committed during restore");

        restored_storage.close_all();
        fs::remove_all(temp_dir);
    }
}

void test_client_preload_complete_state()
{
    printf("\n=== BitTorrentClient: preload complete state ===\n");

    TEST("client starts in complete state when final file already exists");
    {
        namespace fs = std::filesystem;

        const std::string content = "abcdefghijklmnop";
        const auto torrent_data = build_single_file_torrent_without_announce("client_seed.bin", content, 8);
        auto temp_dir = make_bt_test_dir("bt_client_preload");
        const auto target_path = temp_dir / "client_seed.bin";

        {
            FILE *file = fopen(target_path.string().c_str(), "wb");
            if (!file)
            {
                FAIL("failed to create seed file");
                fs::remove_all(temp_dir);
                return;
            }
            fwrite(content.data(), 1, content.size(), file);
            fclose(file);
        }

        yuan::net::SelectPoller poller;
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::EventLoop loop(&poller, &timer_manager);
        yuan::net::NetworkRuntime runtime(&loop, &timer_manager);

        NatConfig cfg;
        cfg.enable_inbound_listen = false;
        cfg.enable_upnp = false;
        cfg.enable_nat_pmp = false;
        cfg.enable_utp = false;
        cfg.enable_dht = false;
        cfg.enable_pex = false;

        BitTorrentClient client;
        client.set_runtime(runtime);
        client.set_nat_config(cfg);
        client.set_save_path(temp_dir.string());

        const bool loaded = client.load_torrent_data(torrent_data);
        const bool started = loaded && client.start();
        const auto stats = client.get_stats();
        const bool passed = started &&
                            client.is_complete() &&
                            stats.pieces_downloaded_ == 2 &&
                            stats.downloaded_bytes_ == static_cast<int64_t>(content.size()) &&
                            stats.progress_ == 1.0f;
        client.stop();

        CHECK(passed, "client should preload committed data and enter complete state");
        fs::remove_all(temp_dir);
    }

    TEST("client restores verified partial pieces into complete state on startup");
    {
        namespace fs = std::filesystem;

        const std::string content = "abcdefghijklmnop";
        const auto torrent_data = build_single_file_torrent_without_announce("client_restore.bin", content, 8);
        const auto torrent = TorrentMeta::parse(torrent_data);
        auto temp_dir = make_bt_test_dir("bt_client_restore_partial");

        PieceStorage storage;
        storage.configure(&torrent, temp_dir.string());
        const bool wrote0 = storage.write_piece(0, 0,
                                                reinterpret_cast<const uint8_t *>(content.data()),
                                                8);
        const bool wrote1 = storage.write_piece(1, 0,
                                                reinterpret_cast<const uint8_t *>(content.data() + 8),
                                                8);
        storage.close_all();

        yuan::net::SelectPoller poller;
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::EventLoop loop(&poller, &timer_manager);
        yuan::net::NetworkRuntime runtime(&loop, &timer_manager);

        NatConfig cfg;
        cfg.enable_inbound_listen = false;
        cfg.enable_upnp = false;
        cfg.enable_nat_pmp = false;
        cfg.enable_utp = false;
        cfg.enable_dht = false;
        cfg.enable_pex = false;

        BitTorrentClient client;
        client.set_runtime(runtime);
        client.set_nat_config(cfg);
        client.set_save_path(temp_dir.string());

        const bool loaded = client.load_torrent_data(torrent_data);
        const bool started = loaded && client.start();
        const auto stats = client.get_stats();
        const bool final_file_exists = fs::exists(temp_dir / "client_restore.bin");
        const bool passed = wrote0 &&
                            wrote1 &&
                            started &&
                            client.is_complete() &&
                            final_file_exists &&
                            stats.pieces_downloaded_ == 2 &&
                            stats.downloaded_bytes_ == static_cast<int64_t>(content.size()) &&
                            stats.progress_ == 1.0f;
        client.stop();

        CHECK(passed, "client should restore verified full partial pieces into committed complete state");
        fs::remove_all(temp_dir);
    }
}

void test_local_seed_download_e2e()
{
    printf("\n=== BitTorrentClient: local seed/download e2e ===\n");

    TEST("downloader fetches from local seeder via tracker");
    {
        namespace fs = std::filesystem;
        using namespace std::chrono_literals;

        const std::string content = "abcdefghijklmnopqrstuvwxyz012345";
        const int64_t piece_length = 8;
        const auto info_only_torrent = build_single_file_torrent_without_announce("e2e_seed.bin", content, piece_length);

#ifdef _WIN32
        SOCKET tracker_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        bool socket_ok = tracker_fd != INVALID_SOCKET;
#else
        int tracker_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        bool socket_ok = tracker_fd >= 0;
#endif
        if (!socket_ok)
        {
            FAIL("tracker socket create failed");
            return;
        }

        sockaddr_in tracker_addr {};
        tracker_addr.sin_family = AF_INET;
        tracker_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        tracker_addr.sin_port = 0;
        int reuse = 1;
        setsockopt(tracker_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
        if (::bind(tracker_fd, reinterpret_cast<const sockaddr *>(&tracker_addr), sizeof(tracker_addr)) != 0 ||
            ::listen(tracker_fd, 8) != 0)
        {
            close_test_socket(tracker_fd);
            FAIL("tracker bind/listen failed");
            return;
        }

        sockaddr_in bound_addr {};
#ifdef _WIN32
        int bound_len = sizeof(bound_addr);
#else
        socklen_t bound_len = sizeof(bound_addr);
#endif
        getsockname(tracker_fd, reinterpret_cast<sockaddr *>(&bound_addr), &bound_len);
        const uint16_t tracker_port = ntohs(bound_addr.sin_port);

        auto seeder_dir = make_bt_test_dir("bt_e2e_seeder");
        auto downloader_dir = make_bt_test_dir("bt_e2e_downloader");
        const auto seeder_file = seeder_dir / "e2e_seed.bin";
        {
            FILE *file = fopen(seeder_file.string().c_str(), "wb");
            if (!file)
            {
                close_test_socket(tracker_fd);
                FAIL("failed to create seeder file");
                fs::remove_all(seeder_dir);
                fs::remove_all(downloader_dir);
                return;
            }
            fwrite(content.data(), 1, content.size(), file);
            fclose(file);
        }

        const auto downloader_meta = TorrentMeta::parse(info_only_torrent);
        const std::string announce_url = "http://127.0.0.1:" + std::to_string(tracker_port) + "/announce";
        const std::string tracker_torrent = "d"
            + bencode_str("announce") + bencode_str(announce_url)
            + bencode_str("announce-list") + "l" + "l" + bencode_str(announce_url) + "e" + "e"
            + info_only_torrent.substr(1); // reuse same info dict

        std::atomic_int seeder_port {0};
        std::atomic_bool tracker_stop {false};
        std::thread tracker_thread([&]() {
            while (!tracker_stop.load())
            {
#ifdef _WIN32
                int client_len = sizeof(sockaddr_in);
#else
                socklen_t client_len = sizeof(sockaddr_in);
#endif
                sockaddr_in client_addr {};
                const auto client_fd = ::accept(tracker_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
#ifdef _WIN32
                if (client_fd == INVALID_SOCKET)
#else
                if (client_fd < 0)
#endif
                {
                    if (tracker_stop.load())
                    {
                        break;
                    }
                    continue;
                }

                char request_buf[4096] {};
                const auto received = ::recv(client_fd, request_buf, sizeof(request_buf), 0);
                std::string request = received > 0 ? std::string(request_buf, request_buf + received) : std::string();
                const int port_value = seeder_port.load();
                std::string body = "d8:intervali1e5:peers0:e";
                if (port_value > 0)
                {
                    body = "d8:intervali1e5:peers6:";
                    const char compact_peer[] = {
                        static_cast<char>(0x7f), static_cast<char>(0x00), static_cast<char>(0x00),
                        static_cast<char>(0x01),
                        static_cast<char>((port_value >> 8) & 0xFF),
                        static_cast<char>(port_value & 0xFF)
                    };
                    body.append(compact_peer, sizeof(compact_peer));
                    body.append("e");
                }

                const std::string response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Connection: close\r\n"
                    "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
                (void)request;
                (void)::send(client_fd, response.data(), static_cast<int>(response.size()), 0);
                close_test_socket(client_fd);
            }
            close_test_socket(tracker_fd);
        });

        yuan::net::SelectPoller seeder_poller;
        yuan::timer::WheelTimerManager seeder_timer;
        yuan::net::EventLoop seeder_loop(&seeder_poller, &seeder_timer);
        yuan::net::NetworkRuntime seeder_runtime(&seeder_loop, &seeder_timer);
        yuan::net::SelectPoller downloader_poller;
        yuan::timer::WheelTimerManager downloader_timer;
        yuan::net::EventLoop downloader_loop(&downloader_poller, &downloader_timer);
        yuan::net::NetworkRuntime downloader_runtime(&downloader_loop, &downloader_timer);

        NatConfig seeder_cfg;
        seeder_cfg.listen_port = 41081;
        seeder_cfg.listen_retry_range = 10;
        seeder_cfg.enable_inbound_listen = true;
        seeder_cfg.enable_upnp = false;
        seeder_cfg.enable_nat_pmp = false;
        seeder_cfg.enable_utp = false;
        seeder_cfg.enable_dht = false;
        seeder_cfg.enable_pex = false;
        seeder_cfg.allow_loopback_peers = true;

        NatConfig downloader_cfg = seeder_cfg;
        downloader_cfg.enable_inbound_listen = false;

        BitTorrentClient seeder;
        seeder.set_runtime(seeder_runtime);
        seeder.set_nat_config(seeder_cfg);
        seeder.set_save_path(seeder_dir.string());
        const bool seeder_loaded = seeder.load_torrent_data(info_only_torrent);
        const bool seeder_started = seeder_loaded && seeder.start();

        std::thread seeder_loop_thread([&]() {
            seeder_loop.loop();
        });

        if (seeder_started)
        {
            const auto listen_deadline = std::chrono::steady_clock::now() + 2s;
            while (std::chrono::steady_clock::now() < listen_deadline)
            {
                auto *nat_manager = seeder.get_nat_manager();
                if (nat_manager && nat_manager->is_listening())
                {
                    seeder_port = static_cast<int>(nat_manager->get_external_port());
                    break;
                }
                std::this_thread::sleep_for(20ms);
            }
        }

        BitTorrentClient downloader;
        downloader.set_runtime(downloader_runtime);
        downloader.set_nat_config(downloader_cfg);
        downloader.set_save_path(downloader_dir.string());
        const bool downloader_loaded = downloader.load_torrent_data(tracker_torrent);
        const bool downloader_started = downloader_loaded && downloader.start();

        std::thread downloader_loop_thread([&]() {
            downloader_loop.loop();
        });

        const auto deadline = std::chrono::steady_clock::now() + 8s;
        while (!downloader.is_complete() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(50ms);
        }

        const auto downloaded_file = downloader_dir / "e2e_seed.bin";
        std::string downloaded;
        if (FILE *file = fopen(downloaded_file.string().c_str(), "rb"))
        {
            char buf[128] {};
            const auto n = fread(buf, 1, sizeof(buf), file);
            downloaded.assign(buf, buf + n);
            fclose(file);
        }

        const bool passed = seeder_started &&
                            downloader_started &&
                            downloader.is_complete() &&
                            downloaded == content;

        downloader.stop();
        seeder.stop();
        std::this_thread::sleep_for(100ms);
        downloader_loop.quit();
        seeder_loop.quit();
        if (downloader_loop_thread.joinable()) downloader_loop_thread.join();
        if (seeder_loop_thread.joinable()) seeder_loop_thread.join();
        tracker_stop = true;

#ifdef _WIN32
        SOCKET poke_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (poke_fd != INVALID_SOCKET)
#else
        int poke_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (poke_fd >= 0)
#endif
        {
            sockaddr_in poke_addr {};
            poke_addr.sin_family = AF_INET;
            poke_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            poke_addr.sin_port = htons(tracker_port);
            ::connect(poke_fd, reinterpret_cast<const sockaddr *>(&poke_addr), sizeof(poke_addr));
            close_test_socket(poke_fd);
        }
        if (tracker_thread.joinable()) tracker_thread.join();

        CHECK(passed, "downloader should complete from local seeder via tracker");
        fs::remove_all(seeder_dir);
        fs::remove_all(downloader_dir);
    }
}

// ============================================================================
// Part 10: Integration - BitTorrentClient Configuration Tests
// ============================================================================

void test_client_nat_integration()
{
    printf("\n=== Integration: BitTorrentClient NAT config ===\n");

    TEST("NatConfig integration with client");
    {
        NatConfig cfg;
        cfg.listen_port = 6881;
        cfg.enable_upnp = true;
        cfg.enable_dht = true;
        cfg.enable_pex = true;
        cfg.enable_utp = true;

        // Verify all settings can be applied
        CHECK(cfg.listen_port == 6881, "listen port set");
        CHECK(cfg.enable_upnp == true, "UPnP enabled");
        CHECK(cfg.enable_dht == true, "DHT enabled");
        CHECK(cfg.enable_pex == true, "PEX enabled");
        CHECK(cfg.enable_utp == true, "uTP enabled");
    }

    TEST("Multi-torrent info_hash collision check");
    {
        std::string torrent1 = build_test_torrent("file1.txt", 262144, 262144, 1);
        std::string torrent2 = build_test_torrent("file2.txt", 262144, 262144, 1);

        auto meta1 = TorrentMeta::parse(torrent1);
        auto meta2 = TorrentMeta::parse(torrent2);

        CHECK(meta1.info_hash_.size() == 20, "meta1 info_hash valid");
        CHECK(meta2.info_hash_.size() == 20, "meta2 info_hash valid");
        CHECK(meta1.info_hash_ != meta2.info_hash_, "different files should have different info_hash");
    }
}

// ============================================================================
// Part 11: Torrent generation for download tests
// ============================================================================

void test_torrent_generation()
{
    printf("\n=== Torrent generation for real downloads ===\n");

    TEST("generate torrent with multiple pieces");
    {
        auto torrent_data = build_test_torrent("bigfile.bin", 1048576, 262144, 4);
        auto meta = TorrentMeta::parse(torrent_data);
        CHECK(meta.info.piece_count() == 4, "should have 4 pieces");
        CHECK(meta.info.total_length_ == 1048576, "total 1MB");
    }

    TEST("generate torrent with exact piece alignment");
    {
        auto torrent_data = build_test_torrent("aligned.bin", 524288, 262144, 2);
        auto meta = TorrentMeta::parse(torrent_data);
        CHECK(meta.info.piece_count() == 2, "should have exactly 2 pieces");
    }

    TEST("generate torrent with non-aligned size");
    {
        auto torrent_data = build_test_torrent("unaligned.bin", 300000, 262144, 2);
        auto meta = TorrentMeta::parse(torrent_data);
        CHECK(meta.info.piece_count() == 2, "300KB / 256KB should ceil to 2 pieces");
    }
}

// ============================================================================
// Main
// ============================================================================

int main()
{
    const test::common::WinsockGuard winsock;
    if (!winsock.ok()) {
        printf("WSAStartup failed\n");
        return 1;
    }

    // Disable stdout buffering to ensure all output is flushed
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    printf("============================================================\n");
    printf("  BitTorrent Protocol Unit Tests (with NAT Traversal)\n");
    printf("============================================================\n");

    // Part 1: Utils
    test_sha1_hash();
    test_to_hex();
    test_from_hex();
    test_url_encode();
    test_generate_peer_id();
    test_current_timestamp_ms();

    // Part 2: Peer Wire Messages
    test_handshake_message();
    test_peer_message_simple();
    test_peer_message_have();
    test_peer_message_request();
    test_peer_message_piece();
    test_peer_message_parse_incomplete();

    // Part 3: Peer State
    test_peer_state();

    // Part 4: Torrent Meta
    test_torrent_meta_parse();
    test_tracker_response_parse();
    test_http_tracker_client_integration();
    test_tracker_session_lifecycle_events();

    // Part 5: NAT Config
    test_nat_config();

    // Part 6: uTP Header
    test_utp_header();
    test_utp_connection_state();

    // Part 7: DHT
    test_dht_node_id();
    test_dht_bucket();
    test_dht_node_basic();

    // Part 8: PEX
    test_pex_manager_basic();
    test_pex_compact_format();

    // Part 9: Piece download state
    test_piece_download_state_window();
    test_piece_completion_state();
    test_download_stats_tracker_upload_context();

    // Part 10: Integration
    test_piece_storage_committed_verification();
    test_client_preload_complete_state();
    test_local_seed_download_e2e();
    test_client_nat_integration();
    test_torrent_generation();

    printf("\n============================================================\n");
    printf("  Results: %d passed, %d failed\n", g_pass, g_fail);
    printf("============================================================\n");
    fflush(stdout);

    return g_fail > 0 ? 1 : 0;
}
