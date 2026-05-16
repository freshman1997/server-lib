#include "smb.h"
#include "buffer/byte_buffer.h"
#include "net/socket/inet_address.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <filesystem>
#include <fstream>


#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

using namespace yuan::net::smb;
using namespace yuan::buffer;

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

static void test_write_le16(std::vector<uint8_t> &buf, size_t offset, uint16_t value)
{
    buf[offset] = static_cast<uint8_t>(value & 0xFF);
    buf[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

static void test_write_le32(std::vector<uint8_t> &buf, size_t offset, uint32_t value)
{
    buf[offset] = static_cast<uint8_t>(value & 0xFF);
    buf[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buf[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    buf[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

static std::vector<uint8_t> utf16_bytes(const std::string &text)
{
    std::vector<uint8_t> out;
    out.reserve(text.size() * 2);
    for (char ch : text) {
        out.push_back(static_cast<uint8_t>(ch));
        out.push_back(0);
    }
    return out;
}

static void append_security_buffer(std::vector<uint8_t> &msg,
                                   size_t descriptor_offset,
                                   const std::vector<uint8_t> &data)
{
    const auto offset = static_cast<uint32_t>(msg.size());
    test_write_le16(msg, descriptor_offset, static_cast<uint16_t>(data.size()));
    test_write_le16(msg, descriptor_offset + 2, static_cast<uint16_t>(data.size()));
    test_write_le32(msg, descriptor_offset + 4, offset);
    msg.insert(msg.end(), data.begin(), data.end());
}

static std::vector<uint8_t> build_test_type3(const std::string &user,
                                             const std::string &domain,
                                             const std::string &workstation,
                                             const std::vector<uint8_t> &nt_response)
{
    std::vector<uint8_t> msg(64, 0);
    std::memcpy(msg.data(), "NTLMSSP\0", 8);
    test_write_le32(msg, 8, NTLMSSP_MESSAGE_TYPE_AUTHENTICATE);
    append_security_buffer(msg, 12, {});
    append_security_buffer(msg, 20, nt_response);
    append_security_buffer(msg, 28, utf16_bytes(domain));
    append_security_buffer(msg, 36, utf16_bytes(user));
    append_security_buffer(msg, 44, utf16_bytes(workstation));
    append_security_buffer(msg, 52, {});
    test_write_le32(msg, 60, NTLMSSP_NEGOTIATE_UNICODE | NTLMSSP_NEGOTIATE_NTLM |
                             NTLMSSP_NEGOTIATE_EXTENDED_SESSIONSECURITY);
    return msg;
}

#define TEST_ASSERT(expr, msg)                                                              \
    do {                                                                                    \
        if (!(expr)) {                                                                      \
            std::cout << "  FAIL: " << msg << " (at line " << __LINE__ << ")" << std::endl; \
            return false;                                                                   \
        }                                                                                   \
    } while (0)

#define RUN_TEST(func)                                       \
    do {                                                     \
        g_tests_run++;                                       \
        std::cout << "  Running: " #func "..." << std::endl; \
        if (func()) {                                        \
            g_tests_passed++;                                \
            std::cout << "  PASS" << std::endl;              \
        } else {                                             \
            g_tests_failed++;                                \
            std::cout << "  FAIL" << std::endl;              \
        }                                                    \
    } while (0)

bool test_smb2_constants()
{
    TEST_ASSERT(SMB2_HEADER_SIZE == 64, "SMB2 header size should be 64");
    TEST_ASSERT(static_cast<uint16_t>(Smb2Command::NEGOTIATE) == 0x0000, "NEGOTIATE command");
    TEST_ASSERT(static_cast<uint16_t>(Smb2Command::SESSION_SETUP) == 0x0001, "SESSION_SETUP command");
    TEST_ASSERT(static_cast<uint16_t>(Smb2Command::LOGOFF) == 0x0002, "LOGOFF command");
    TEST_ASSERT(static_cast<uint16_t>(Smb2Command::TREE_CONNECT) == 0x0003, "TREE_CONNECT command");
    TEST_ASSERT(static_cast<uint16_t>(Smb2Command::TREE_DISCONNECT) == 0x0004, "TREE_DISCONNECT command");
    TEST_ASSERT(static_cast<uint16_t>(Smb2Command::CREATE) == 0x0005, "CREATE command");
    TEST_ASSERT(static_cast<uint16_t>(Smb2Command::CLOSE) == 0x0006, "CLOSE command");
    TEST_ASSERT(static_cast<uint16_t>(Smb2Command::READ) == 0x0008, "READ command");
    TEST_ASSERT(static_cast<uint16_t>(Smb2Command::WRITE) == 0x0009, "WRITE command");
    TEST_ASSERT(static_cast<uint16_t>(DialectRevision::SMB_2_002) == 0x0202, "SMB 2.002 dialect");
    TEST_ASSERT(static_cast<uint16_t>(DialectRevision::SMB_2_1) == 0x0210, "SMB 2.1 dialect");
    TEST_ASSERT(static_cast<uint16_t>(DialectRevision::SMB_3_0) == 0x0300, "SMB 3.0 dialect");
    TEST_ASSERT(static_cast<uint16_t>(DialectRevision::SMB_3_0_2) == 0x0302, "SMB 3.0.2 dialect");
    TEST_ASSERT(static_cast<uint16_t>(DialectRevision::SMB_3_1_1) == 0x0311, "SMB 3.1.1 dialect");
    return true;
}

bool test_smb2_header_encode_decode()
{
    Smb2Header header;
    std::memset(&header, 0, sizeof(header));
    header.protocol_id = SMB2_PROTOCOL_ID;
    header.structure_size = 64;
    header.credit_charge = 1;
    header.status = 0;
    header.command = static_cast<uint16_t>(Smb2Command::NEGOTIATE);
    header.credit_request = 1;
    header.flags = 0;
    header.message_id = 0;
    header.session_id = 0;
    header.tree_id = 0;

    ByteBuffer encoded = Smb2Codec::encode_header(header);
    TEST_ASSERT(encoded.readable_bytes() >= SMB2_HEADER_SIZE, "Encoded header should be at least 64 bytes");

    auto span = encoded.readable_span();
    auto decoded = Smb2Codec::decode_header(
        reinterpret_cast<const uint8_t *>(span.data()), span.size());
    TEST_ASSERT(decoded.has_value(), "Header decode should succeed");
    TEST_ASSERT(decoded->command == static_cast<uint16_t>(Smb2Command::NEGOTIATE),
                "Decoded command should match");
    TEST_ASSERT(decoded->credit_charge == 1, "Decoded credit_charge should match");
    TEST_ASSERT(decoded->message_id == 0, "Decoded message_id should match");

    return true;
}

bool test_smb2_is_header_detection()
{
    uint8_t valid[64] = {};
    valid[0] = 0xFE;
    valid[1] = 'S';
    valid[2] = 'M';
    valid[3] = 'B';

    TEST_ASSERT(Smb2Codec::is_smb2_header(valid, sizeof(valid)),
                "Should detect valid SMB2 header");

    uint8_t invalid[64] = {};
    invalid[0] = 0xFF;
    invalid[1] = 'S';
    invalid[2] = 'M';
    invalid[3] = 'B';

    TEST_ASSERT(!Smb2Codec::is_smb2_header(invalid, sizeof(invalid)),
                "Should reject SMB1 header as SMB2");

    TEST_ASSERT(!Smb2Codec::is_smb2_header(valid, 4),
                "Should reject too short data");

    return true;
}

bool test_netbios_encode_decode()
{
    ByteBuffer frame = SmbNetbios::encode(1024);
    TEST_ASSERT(frame.readable_bytes() >= 4, "NetBIOS frame should have 4-byte header");

    auto span = frame.readable_span();
    const uint8_t *data = reinterpret_cast<const uint8_t *>(span.data());

    auto hdr = SmbNetbios::decode(data, frame.readable_bytes());
    TEST_ASSERT(hdr.has_value(), "NetBIOS decode should succeed");
    TEST_ASSERT(hdr->length == 1024, "Decoded length should be 1024");

    return true;
}

bool test_netbios_split_messages()
{
    ByteBuffer buf;

    ByteBuffer msg1 = SmbNetbios::encode(10);
    ByteBuffer payload1(10);
    for (int i = 0; i < 10; ++i)
        payload1.append_u8(static_cast<uint8_t>(i));
    msg1.append(payload1);
    buf.append(msg1);

    ByteBuffer msg2 = SmbNetbios::encode(5);
    ByteBuffer payload2(5);
    for (int i = 0; i < 5; ++i)
        payload2.append_u8(static_cast<uint8_t>(i + 10));
    msg2.append(payload2);
    buf.append(msg2);

    auto messages = SmbNetbios::split_messages(buf);
    TEST_ASSERT(messages.has_value(), "split_messages should succeed");
    TEST_ASSERT(messages->size() == 2, "Should split into 2 messages");

    return true;
}

bool test_smb1_negotiate_detection()
{
    uint8_t smb1_data[] = {
        0xFF, 'S', 'M', 'B', 0x72,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00,
        0x00,
        0x00, 0x00,
        0x0E, 0x00,
        0x02, 'S', 'M', 'B', ' ', '2', '.', '0', '0', '2', 0x00,
        0x02, 'S', 'M', 'B', ' ', '2', '.', '1', 0x00
    };

    TEST_ASSERT(Smb1Negotiate::is_smb1_negotiate(smb1_data, sizeof(smb1_data)),
                "Should detect SMB1 negotiate");

    auto req = Smb1Negotiate::decode(smb1_data, sizeof(smb1_data));
    TEST_ASSERT(req.has_value(), "Should decode SMB1 negotiate");
    TEST_ASSERT(req->supports_smb2, "Should detect SMB2 support in dialects");
    TEST_ASSERT(req->dialects.size() >= 1, "Should have at least 1 dialect");

    return true;
}

bool test_smb1_redirect_response()
{
    auto resp = Smb1Negotiate::build_smb2_negotiate_redirect("TEST-SERVER");
    TEST_ASSERT(resp.readable_bytes() > 0, "Should produce non-empty response");

    auto span = resp.readable_span();
    const uint8_t *data = reinterpret_cast<const uint8_t *>(span.data());
    TEST_ASSERT(data[0] == 0xFF, "Response should start with 0xFF");
    TEST_ASSERT(data[1] == 'S' && data[2] == 'M' && data[3] == 'B',
                "Response should have SMB magic");

    return true;
}

bool test_le_read_write()
{
    uint8_t buf16[2] = {};
    uint16_t val16 = 0x1234;
    std::memcpy(buf16, &val16, 2);

    uint16_t read_val = Smb2Codec::read_le16(buf16);
    TEST_ASSERT(read_val == 0x1234, "read_le16 should read 0x1234");

    uint8_t buf32[4] = {};
    uint32_t val32 = 0x12345678;
    std::memcpy(buf32, &val32, 4);

    uint32_t read_val32 = Smb2Codec::read_le32(buf32);
    TEST_ASSERT(read_val32 == 0x12345678, "read_le32 should read 0x12345678");

    uint8_t buf64[8] = {};
    uint64_t val64 = 0x0102030405060708ULL;
    std::memcpy(buf64, &val64, 8);

    uint64_t read_val64 = Smb2Codec::read_le64(buf64);
    TEST_ASSERT(read_val64 == 0x0102030405060708ULL, "read_le64 should read correct value");

    return true;
}

bool test_utf8_utf16le_conversion()
{
    std::string ascii = "Hello";
    auto u16 = Smb2Codec::utf8_to_utf16le(ascii);
    auto back = Smb2Codec::utf16le_to_utf8(u16);
    TEST_ASSERT(back == ascii, "Roundtrip ASCII should match");

    std::string path = "test\\folder\\file.txt";
    auto u16path = Smb2Codec::utf8_to_utf16le(path);
    auto backpath = Smb2Codec::utf16le_to_utf8(u16path);
    TEST_ASSERT(backpath == path, "Roundtrip path should match");

    return true;
}

bool test_filetime_now()
{
    uint64_t ft = Smb2Codec::filetime_now();
    TEST_ASSERT(ft > 0, "filetime_now should return non-zero");
    uint64_t epoch_2020 = 132224352000000000ULL;
    uint64_t epoch_2030 = 135527616000000000ULL;
    TEST_ASSERT(ft > epoch_2020, "filetime should be after 2020");
    TEST_ASSERT(ft < epoch_2030, "filetime should be before 2030");
    return true;
}

bool test_error_response()
{
    Smb2Header req;
    std::memset(&req, 0, sizeof(req));
    req.protocol_id = SMB2_PROTOCOL_ID;
    req.command = static_cast<uint16_t>(Smb2Command::NEGOTIATE);
    req.message_id = 42;

    auto resp = Smb2Codec::build_error_response(req, NtStatus::ACCESS_DENIED);
    TEST_ASSERT(resp.readable_bytes() >= SMB2_HEADER_SIZE, "Error response should have header");

    auto span = resp.readable_span();
    auto hdr = Smb2Codec::decode_header(
        reinterpret_cast<const uint8_t *>(span.data()), span.size());
    TEST_ASSERT(hdr.has_value(), "Should decode error response header");
    TEST_ASSERT(hdr->status == static_cast<uint32_t>(NtStatus::ACCESS_DENIED),
                "Error response status should be ACCESS_DENIED");

    return true;
}

bool test_server_config_defaults()
{
    SmbServerConfig config;
    TEST_ASSERT(config.port == 445, "Default port should be 445");
    TEST_ASSERT(config.max_transact_size == 1048576, "Default max_transact_size");
    TEST_ASSERT(config.max_read_size == 1048576, "Default max_read_size");
    TEST_ASSERT(config.max_write_size == 1048576, "Default max_write_size");
    TEST_ASSERT(config.max_sessions == 1024, "Default max_sessions");
    TEST_ASSERT(config.enable_smb1_fallback == true, "Default enable_smb1_fallback");
    TEST_ASSERT(config.supported_dialects.size() == 5, "Default 5 dialects");
    TEST_ASSERT(config.supported_dialects[0] == DialectRevision::SMB_2_002, "First dialect");
    TEST_ASSERT(config.supported_dialects[4] == DialectRevision::SMB_3_1_1, "Last dialect");

    return true;
}

bool test_share_config()
{
    SmbShareConfig cfg;
    cfg.name = "public";
    cfg.type = ShareType::DISK;
    cfg.path = "/tmp/smb_test";
    cfg.comment = "Public share";

    TEST_ASSERT(cfg.name == "public", "Share name");
    TEST_ASSERT(cfg.type == ShareType::DISK, "Share type");
    TEST_ASSERT(cfg.max_uses == -1, "Default unlimited uses");

    return true;
}

bool test_share_manager()
{
    SmbShareManager mgr;

    SmbShareConfig cfg;
    cfg.name = "testshare";
    cfg.type = ShareType::DISK;
    cfg.path = "/tmp/smb_share_test";
    cfg.comment = "Test share";

    mgr.add_share(cfg);

    SmbShare *share = mgr.find_share("testshare");
    TEST_ASSERT(share != nullptr, "Should find added share");
    TEST_ASSERT(share->name() == "testshare", "Share name should match");
    TEST_ASSERT(share->type() == ShareType::DISK, "Share type should match");
    TEST_ASSERT(share->comment() == "Test share", "Share comment should match");

    SmbShare *notfound = mgr.find_share("nonexistent");
    TEST_ASSERT(notfound == nullptr, "Should not find nonexistent share");

    mgr.remove_share("testshare");
    SmbShare *removed = mgr.find_share("testshare");
    TEST_ASSERT(removed == nullptr, "Should not find removed share");

    return true;
}

bool test_session_manager()
{
    SmbSessionManager mgr;

    SmbSession *session = mgr.create_session(nullptr);
    TEST_ASSERT(session != nullptr, "Should create session");
    TEST_ASSERT(session->session_id() > 0, "Session ID should be positive");
    TEST_ASSERT(session->state() == SmbSession::State::connected, "Initial state should be connected");

    uint64_t sid = session->session_id();
    SmbSession *found = mgr.find_session(sid);
    TEST_ASSERT(found == session, "Should find session by ID");

    session->set_state(SmbSession::State::negotiating);
    TEST_ASSERT(found->state() == SmbSession::State::negotiating, "State should update");

    mgr.remove_session(sid);
    SmbSession *gone = mgr.find_session(sid);
    TEST_ASSERT(gone == nullptr, "Should not find removed session");

    return true;
}

bool test_session_tree_connections()
{
    SmbSessionManager mgr;
    SmbSession *session = mgr.create_session(nullptr);

    TreeConnection tc;
    tc.share_name = "test";
    tc.tree_id = 0;
    uint32_t tid = session->add_tree_connection(std::move(tc));
    TEST_ASSERT(tid > 0, "Tree ID should be positive");

    TreeConnection *found = session->find_tree(tid);
    TEST_ASSERT(found != nullptr, "Should find tree connection");
    TEST_ASSERT(found->share_name == "test", "Tree share name should match");

    auto ids = session->all_tree_ids();
    TEST_ASSERT(ids.size() == 1, "Should have 1 tree");
    TEST_ASSERT(ids[0] == tid, "Tree ID should match");

    session->remove_tree(tid);
    TreeConnection *removed = session->find_tree(tid);
    TEST_ASSERT(removed == nullptr, "Should not find removed tree");

    mgr.remove_session(session->session_id());
    return true;
}

bool test_session_crypto_keys()
{
    SmbSessionManager mgr;
    SmbSession *session = mgr.create_session(nullptr);

    std::vector<uint8_t> signing_key = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    session->set_signing_key(signing_key);
    TEST_ASSERT(session->is_signed(), "Session should be signed after setting key");
    TEST_ASSERT(session->signing_key() == signing_key, "Signing key should match");

    std::vector<uint8_t> enc_key = { 0xAA, 0xBB, 0xCC, 0xDD };
    session->set_encryption_key(enc_key);
    TEST_ASSERT(session->is_encrypted(), "Session should be encrypted after setting key");

    std::vector<uint8_t> enc_iv = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    session->set_encryption_iv(enc_iv);
    TEST_ASSERT(session->encryption_iv() == enc_iv, "Encryption IV should match");

    std::vector<uint8_t> dec_key = { 0x11, 0x22, 0x33, 0x44 };
    session->set_decryption_key(dec_key);
    TEST_ASSERT(session->decryption_key() == dec_key, "Decryption key should match");

    std::vector<uint8_t> dec_iv = { 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 };
    session->set_decryption_iv(dec_iv);
    TEST_ASSERT(session->decryption_iv() == dec_iv, "Decryption IV should match");

    mgr.remove_session(session->session_id());
    return true;
}

bool test_lock_manager_basic()
{
    SmbLockManager mgr;

    FileId fid;
    fid.persistent = 1;
    fid.volatile_id = 1;

    auto status = mgr.request_lock(fid, 100, 1, 0, 1024, true);
    TEST_ASSERT(status == NtStatus::SUCCESS, "First exclusive lock should succeed");

    auto status2 = mgr.request_lock(fid, 200, 2, 0, 1024, true);
    TEST_ASSERT(status2 != NtStatus::SUCCESS, "Overlapping exclusive lock should fail");

    auto status3 = mgr.request_lock(fid, 100, 1, 2048, 1024, true);
    TEST_ASSERT(status3 == NtStatus::SUCCESS, "Non-overlapping lock should succeed");

    auto release = mgr.release_lock(fid, 100, 0, 1024);
    TEST_ASSERT(release == NtStatus::SUCCESS, "Lock release should succeed");

    auto status4 = mgr.request_lock(fid, 200, 2, 0, 1024, true);
    TEST_ASSERT(status4 == NtStatus::SUCCESS, "Lock after release should succeed");

    return true;
}

bool test_lock_manager_oplock()
{
    SmbLockManager mgr;

    FileId fid;
    fid.persistent = 10;
    fid.volatile_id = 10;

    auto status = mgr.request_oplock(fid, 100, SMB2_OPLOCK_LEVEL_II);
    TEST_ASSERT(status == NtStatus::SUCCESS, "Oplock request should succeed");

    auto oplock = mgr.get_oplock(fid);
    TEST_ASSERT(oplock.has_value(), "Should find oplock");
    TEST_ASSERT(oplock->current_level == SMB2_OPLOCK_LEVEL_II, "Oplock level should match");

    auto ack = mgr.ack_oplock_break(fid, 100, SMB2_OPLOCK_LEVEL_NONE);
    TEST_ASSERT(ack == NtStatus::SUCCESS, "Oplock break ack should succeed");

    mgr.remove_oplock(fid);
    auto gone = mgr.get_oplock(fid);
    TEST_ASSERT(!gone.has_value(), "Oplock should be removed");

    return true;
}

bool test_lock_manager_lease()
{
    SmbLockManager mgr;

    FileId fid;
    fid.persistent = 20;
    fid.volatile_id = 20;

    uint8_t lease_key[SMB2_LEASE_KEY_SIZE] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

    auto status = mgr.request_lease(fid, 100, lease_key, SMB2_LEASE_READ_CACHING);
    TEST_ASSERT(status == NtStatus::SUCCESS, "Lease request should succeed");

    auto lease = mgr.get_lease(lease_key);
    TEST_ASSERT(lease.has_value(), "Should find lease");
    TEST_ASSERT(lease->current_state == SMB2_LEASE_READ_CACHING, "Lease state should match");

    mgr.remove_lease(fid);
    auto gone = mgr.get_lease(lease_key);
    TEST_ASSERT(!gone.has_value(), "Lease should be removed");

    return true;
}

bool test_pipe_manager_basic()
{
    SmbPipeManager mgr;

    mgr.register_pipe("test_pipe");
    TEST_ASSERT(mgr.exists("test_pipe"), "Registered pipe should exist");
    TEST_ASSERT(!mgr.exists("nonexistent"), "Non-registered pipe should not exist");

    uint64_t handle = mgr.open_pipe("test_pipe", 1);
    TEST_ASSERT(handle > 0, "Open pipe should return valid handle");

    PipeInstance *inst = mgr.find_pipe(handle);
    TEST_ASSERT(inst != nullptr, "Should find pipe instance");
    TEST_ASSERT(inst->name == "test_pipe", "Pipe name should match");

    mgr.close_pipe(handle);
    PipeInstance *closed = mgr.find_pipe(handle);
    TEST_ASSERT(closed == nullptr, "Closed pipe should not be found");

    mgr.unregister_pipe("test_pipe");
    TEST_ASSERT(!mgr.exists("test_pipe"), "Unregistered pipe should not exist");

    return true;
}

bool test_pipe_manager_custom_handlers()
{
    SmbPipeManager mgr;

    bool read_called = false;
    bool write_called = false;

    mgr.register_pipe("custom",
                      [&](const std::string & name, uint64_t handle, uint32_t len)->std::vector<uint8_t> {
            read_called = true;
            return {1, 2, 3, 4};
                      },
                      [&](const std::string & name, uint64_t handle, const uint8_t * data, uint32_t len)->uint32_t {
            write_called = true;
            return len;
    });

    uint64_t h = mgr.open_pipe("custom", 1);
    auto data = mgr.read_pipe(h, 100);
    TEST_ASSERT(read_called, "Custom read handler should be called");
    TEST_ASSERT(data.size() == 4, "Read data size should match");

    uint32_t written = mgr.write_pipe(h, reinterpret_cast<const uint8_t *>("test"), 4);
    TEST_ASSERT(write_called, "Custom write handler should be called");
    TEST_ASSERT(written == 4, "Written bytes should match");

    mgr.close_pipe(h);
    return true;
}

bool test_dfs_resolver()
{
    SmbDfsResolver resolver;

    DfsReferral ref;
    ref.dfs_path = "\\\\server\\share\\path";
    ref.target_server = "fs1.example.com";
    ref.target_share = "data";
    ref.target_path = "\\path";

    resolver.add_referral(ref);

    auto result = resolver.resolve("\\\\server\\share\\path");
    TEST_ASSERT(result.has_value(), "Should resolve known DFS path");
    TEST_ASSERT(result->target_server == "fs1.example.com", "Target server should match");

    auto no_result = resolver.resolve("\\\\unknown\\path");
    TEST_ASSERT(!no_result.has_value(), "Should not resolve unknown DFS path");

    resolver.remove_referral("\\\\server\\share\\path");
    auto removed = resolver.resolve("\\\\server\\share\\path");
    TEST_ASSERT(!removed.has_value(), "Removed referral should not resolve");

    return true;
}

bool test_change_notifier()
{
    SmbChangeNotifier notifier;

    bool callback_called = false;
    uint64_t watch_id = notifier.register_watch(
        FileId{ 1, 1 }, 0x1F,
        [&](const FileId &fid, uint32_t action) {
            callback_called = true;
        });

    TEST_ASSERT(watch_id > 0, "Watch ID should be positive");

    notifier.notify_change(FileId{ 1, 1 }, 0x01);
    TEST_ASSERT(callback_called, "Callback should be called on change");

    notifier.unregister_watch(watch_id);
    return true;
}

bool test_ntlm_auth()
{
    SmbNtlmAuth ntlm("TESTSERVER", "DOMAIN");

    TEST_ASSERT(ntlm.is_complete() == false, "NTLM auth should not be complete initially");

    const std::vector<uint8_t> type1 = {
        'N', 'T', 'L', 'M', 'S', 'S', 'P', '\0',
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00
    };
    auto type2 = ntlm.process_inbound_token(type1);
    TEST_ASSERT(!type2.empty(), "Should produce Type2 challenge");

    return true;
}

bool test_ntlmv2_password_proof_auth()
{
    SmbNtlmAuth ntlm("TESTSERVER", "DOMAIN");
    ntlm.set_password_lookup([](const std::string &user, const std::string &domain)->std::optional<std::string> {
        return (user == "alice" && domain == "DOMAIN") ? std::optional<std::string>("secret") : std::nullopt;
    });
    ntlm.set_credentials_db([](const std::string &user, const std::string &domain, const std::string &password)->bool {
        return user == "alice" && domain == "DOMAIN" && password == "secret";
    });

    const std::vector<uint8_t> type1 = {
        'N', 'T', 'L', 'M', 'S', 'S', 'P', '\0',
        0x01, 0x00, 0x00, 0x00,
        static_cast<uint8_t>(NTLMSSP_NEGOTIATE_UNICODE & 0xFF),
        static_cast<uint8_t>((NTLMSSP_NEGOTIATE_NTLM >> 8) & 0xFF),
        static_cast<uint8_t>((NTLMSSP_NEGOTIATE_EXTENDED_SESSIONSECURITY >> 16) & 0xFF),
        0x00
    };
    auto type2 = ntlm.process_inbound_token(type1);
    TEST_ASSERT(type2.size() >= 48, "NTLM Type2 should include challenge and target info");

    std::array<uint8_t, 8> challenge{};
    std::memcpy(challenge.data(), type2.data() + 24, challenge.size());
    const uint16_t target_info_len = static_cast<uint16_t>(type2[40] | (type2[41] << 8));
    const uint32_t target_info_offset = static_cast<uint32_t>(type2[44]) |
                                        (static_cast<uint32_t>(type2[45]) << 8) |
                                        (static_cast<uint32_t>(type2[46]) << 16) |
                                        (static_cast<uint32_t>(type2[47]) << 24);
    TEST_ASSERT(target_info_offset + target_info_len <= type2.size(), "Type2 target info should be in range");

    std::vector<uint8_t> blob(type2.begin() + target_info_offset,
                              type2.begin() + target_info_offset + target_info_len);
    auto response = SmbNtlmAuth::ntlmv2_response("secret", "alice", "DOMAIN", challenge, blob);
    TEST_ASSERT(SmbNtlmAuth::verify_ntlmv2_response("secret", "alice", "DOMAIN", challenge, response),
                "Static NTLMv2 verifier should accept correct response");
    TEST_ASSERT(!SmbNtlmAuth::verify_ntlmv2_response("bad", "alice", "DOMAIN", challenge, response),
                "Static NTLMv2 verifier should reject wrong password");

    auto type3 = build_test_type3("alice", "DOMAIN", "WORKSTATION", response);
    auto final = ntlm.process_inbound_token(type3);
    TEST_ASSERT(final.empty(), "NTLM authenticate should not need outbound token");
    TEST_ASSERT(ntlm.is_complete(), "NTLM auth should be complete after Type3");
    TEST_ASSERT(ntlm.result().success, "NTLM auth should accept valid NTLMv2 proof");
    TEST_ASSERT(!ntlm.result().session_key.empty(), "NTLM auth should derive a session key");

    SmbNtlmAuth wrong("TESTSERVER", "DOMAIN");
    wrong.set_password_lookup([](const std::string &, const std::string &)->std::optional<std::string> {
        return std::string("bad");
    });
    auto wrong_type2 = wrong.process_inbound_token(type1);
    TEST_ASSERT(!wrong_type2.empty(), "Wrong-password auth should still issue challenge");
    auto rejected = wrong.process_inbound_token(type3);
    TEST_ASSERT(rejected.empty(), "Rejected auth should not emit token");
    TEST_ASSERT(wrong.is_complete(), "Rejected auth should complete as failed");
    TEST_ASSERT(!wrong.result().success, "Wrong password should reject NTLMv2 proof");

    return true;
}

bool test_ntlmv2_nt_hash_proof_auth()
{
    SmbNtlmAuth ntlm("TESTSERVER", "DOMAIN");
    ntlm.set_nt_hash_lookup([](const std::string &user, const std::string &domain)->std::optional<std::string> {
        if (user != "alice" || domain != "DOMAIN") {
            return std::nullopt;
        }
        return std::string("nthash:878d8014606cda29677a44efa1353fc7");
    });
    ntlm.set_credentials_db([](const std::string &user, const std::string &domain, const std::string &password)->bool {
        return user == "alice" && domain == "DOMAIN" && password.empty();
    });

    const std::vector<uint8_t> type1 = {
        'N', 'T', 'L', 'M', 'S', 'S', 'P', '\0',
        0x01, 0x00, 0x00, 0x00,
        static_cast<uint8_t>(NTLMSSP_NEGOTIATE_UNICODE & 0xFF),
        static_cast<uint8_t>((NTLMSSP_NEGOTIATE_NTLM >> 8) & 0xFF),
        static_cast<uint8_t>((NTLMSSP_NEGOTIATE_EXTENDED_SESSIONSECURITY >> 16) & 0xFF),
        0x00
    };
    auto type2 = ntlm.process_inbound_token(type1);
    TEST_ASSERT(type2.size() >= 48, "NT hash auth should issue Type2 challenge");

    std::array<uint8_t, 8> challenge{};
    std::memcpy(challenge.data(), type2.data() + 24, challenge.size());
    const uint16_t target_info_len = static_cast<uint16_t>(type2[40] | (type2[41] << 8));
    const uint32_t target_info_offset = static_cast<uint32_t>(type2[44]) |
                                        (static_cast<uint32_t>(type2[45]) << 8) |
                                        (static_cast<uint32_t>(type2[46]) << 16) |
                                        (static_cast<uint32_t>(type2[47]) << 24);
    TEST_ASSERT(target_info_offset + target_info_len <= type2.size(), "Type2 target info should be in range");

    std::vector<uint8_t> blob(type2.begin() + target_info_offset,
                              type2.begin() + target_info_offset + target_info_len);
    auto response = SmbNtlmAuth::ntlmv2_response("nthash-secret", "alice", "DOMAIN", challenge, blob);
    auto type3 = build_test_type3("alice", "DOMAIN", "WORKSTATION", response);
    auto final = ntlm.process_inbound_token(type3);
    TEST_ASSERT(final.empty(), "NT hash auth should not need outbound token after Type3");
    TEST_ASSERT(ntlm.is_complete(), "NT hash auth should complete after Type3");
    TEST_ASSERT(ntlm.result().success, "NT hash auth should accept valid NTLMv2 proof");
    TEST_ASSERT(!ntlm.result().session_key.empty(), "NT hash auth should derive a session key");

    return true;
}

bool test_spnego_auth()
{
    SmbSpnegoAuth spnego("MYSERVER", "WORKGROUP");

    TEST_ASSERT(!spnego.is_complete(), "SPNEGO auth should not be complete initially");

    auto initial = spnego.process_inbound_token({});
    TEST_ASSERT(!initial.empty(), "Should produce initial SPNEGO token");

    const uint8_t expected[] = {
        0x60, 0x1A,
        0x06, 0x06, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x02,
        0x30, 0x10,
        0xA0, 0x0E,
        0x30, 0x0C,
        0x06, 0x0A, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82,
        0x37, 0x02, 0x02, 0x0A
    };
    TEST_ASSERT(initial.size() == sizeof(expected),
                "Initial SPNEGO token size mismatch");
    TEST_ASSERT(std::memcmp(initial.data(), expected, sizeof(expected)) == 0,
                "Initial SPNEGO token bytes must match expected NegTokenInit");

    return true;
}

bool test_spnego_auth_accepts_oid_only_init()
{
    SmbSpnegoAuth spnego("MYSERVER", "WORKGROUP");

    const std::vector<uint8_t> token = {
        0xA0, 0x16,
        0x30, 0x14,
        0xA0, 0x0E,
        0x30, 0x0C,
        0x06, 0x0A, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82,
        0x37, 0x02, 0x02, 0x0A,
        0xA2, 0x02, 0x04, 0x00
    };

    auto response = spnego.process_inbound_token(token);
    TEST_ASSERT(!response.empty(), "SPNEGO should accept Samba OID-only init token");
    TEST_ASSERT(spnego.state() == AuthState::challenge_sent,
                "OID-only init should advance auth state to challenge_sent");

    TEST_ASSERT(response[0] == 0xA1,
                "Response to client NegTokenInit must be NegTokenResp (tag 0xA1)");
    TEST_ASSERT(response.size() >= 4,
                "NegTokenResp must have at least tag, length, inner seq");

    bool found_neg_result = false;
    for (size_t i = 0; i + 5 <= response.size(); ++i) {
        if (response[i] == 0xA0 && response[i + 2] == 0x0A &&
            response[i + 3] == 0x01) {
            TEST_ASSERT(response[i + 4] == 0x01,
                        "negResult must be accept-incomplete (1) during challenge");
            found_neg_result = true;
            break;
        }
    }
    TEST_ASSERT(found_neg_result,
                "NegTokenResp must contain negResult with ENUMERATED tag (0x0A) and value accept-incomplete");

    const uint8_t ntlmssp_sig[] = { 'N', 'T', 'L', 'M', 'S', 'S', 'P', '\0' };
    bool found_ntlmssp = false;
    for (size_t i = 0; i + 8 <= response.size(); ++i) {
        if (std::memcmp(response.data() + i, ntlmssp_sig, 8) == 0) {
            found_ntlmssp = true;
            uint8_t msg_type = response[i + 8] | (response[i + 9] << 8);
            TEST_ASSERT(msg_type == 0x0002,
                        "NegTokenResp mechToken must contain NTLM Type 2 (Challenge)");
            break;
        }
    }
    TEST_ASSERT(found_ntlmssp,
                "NegTokenResp must contain NTLMSSP challenge blob");

    return true;
}

bool test_spnego_auth_extracts_mechtoken_from_der_init()
{
    SmbSpnegoAuth spnego("MYSERVER", "WORKGROUP");

    const std::vector<uint8_t> token = {
        0x60, 0x30,
        0x06, 0x06, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x02,
        0xA0, 0x26,
        0x30, 0x24,
        0xA0, 0x0E,
        0x30, 0x0C,
        0x06, 0x0A, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82,
        0x37, 0x02, 0x02, 0x0A,
        0xA2, 0x12,
        0x04, 0x10,
        'N', 'T', 'L', 'M', 'S', 'S', 'P', '\0',
        0x01, 0x00, 0x00, 0x00,
        0x07, 0x82, 0x08, 0xA0
    };

    const uint8_t expected_mechtoken[] = {
        'N', 'T', 'L', 'M', 'S', 'S', 'P', '\0',
        0x01, 0x00, 0x00, 0x00,
        0x07, 0x82, 0x08, 0xA0
    };

    auto extracted = spnego.extract_mech_token_for_test(token);
    TEST_ASSERT(extracted.size() == sizeof(expected_mechtoken),
                "DER NegTokenInit mechToken size mismatch");
    TEST_ASSERT(std::memcmp(extracted.data(), expected_mechtoken, sizeof(expected_mechtoken)) == 0,
                "DER NegTokenInit mechToken bytes must match expected NTLM NEGOTIATE");

    auto response = spnego.process_inbound_token(token);
    TEST_ASSERT(!response.empty(), "DER NegTokenInit with mechToken should produce a challenge response");
    TEST_ASSERT(spnego.state() == AuthState::challenge_sent,
                "DER NegTokenInit should drive NTLM challenge generation");

    TEST_ASSERT(response[0] == 0xA1,
                "Response must be NegTokenResp (tag 0xA1)");

    bool found_neg_result = false;
    for (size_t i = 0; i + 5 <= response.size(); ++i) {
        if (response[i] == 0xA0 && response[i + 2] == 0x0A &&
            response[i + 3] == 0x01) {
            TEST_ASSERT(response[i + 4] == 0x01,
                        "negResult must be accept-incomplete (1) during challenge");
            found_neg_result = true;
            break;
        }
    }
    TEST_ASSERT(found_neg_result,
                "NegTokenResp must contain negResult with ENUMERATED tag (0x0A)");

    const uint8_t ntlmssp_sig[] = { 'N', 'T', 'L', 'M', 'S', 'S', 'P', '\0' };
    bool found_challenge = false;
    for (size_t i = 0; i + 10 <= response.size(); ++i) {
        if (std::memcmp(response.data() + i, ntlmssp_sig, 8) == 0) {
            uint8_t msg_type = response[i + 8] | (response[i + 9] << 8);
            TEST_ASSERT(msg_type == 0x0002,
                        "Response must contain NTLM Type 2 (Challenge)");
            found_challenge = true;
            break;
        }
    }
    TEST_ASSERT(found_challenge,
                "NegTokenResp must contain NTLMSSP challenge blob");

    return true;
}

bool test_crypto_openssl_sha512()
{
    SmbCryptoOpenSSL crypto;

    std::vector<uint8_t> data = { 1, 2, 3, 4, 5 };
    auto hash = crypto.sha512(data.data(), data.size());
    TEST_ASSERT(hash.size() == 64, "SHA-512 should produce 64-byte hash");

    auto hash2 = crypto.sha512(data.data(), data.size());
    TEST_ASSERT(hash == hash2, "Same input should produce same hash");

    return true;
}

bool test_crypto_openssl_hmac()
{
    SmbCryptoOpenSSL crypto;

    std::vector<uint8_t> key(16, 0xAA);
    std::vector<uint8_t> data = { 1, 2, 3, 4, 5, 6, 7, 8 };
    auto mac = crypto.hmac_sha256(key, data.data(), data.size());
    TEST_ASSERT(mac.size() == 32, "HMAC-SHA256 should produce 32-byte output");

    auto mac2 = crypto.hmac_sha256(key, data.data(), data.size());
    TEST_ASSERT(mac == mac2, "Same input should produce same HMAC");

    return true;
}

bool test_crypto_sign_verify()
{
    SmbCryptoOpenSSL crypto;

    std::vector<uint8_t> key(16, 0x42);
    std::vector<uint8_t> data(128, 0x55);

    auto sig = crypto.sign(key, data.data(), data.size());
    TEST_ASSERT(!sig.empty(), "Sign should produce signature");

    bool verified = crypto.verify(key, data.data(), data.size(), sig.data());
    TEST_ASSERT(verified, "Signature should verify");

    data[0] = ~data[0];
    bool tampered = crypto.verify(key, data.data(), data.size(), sig.data());
    TEST_ASSERT(!tampered, "Tampered data should fail verification");

    return true;
}

bool test_crypto_encrypt_decrypt()
{
    SmbCryptoOpenSSL crypto;

    std::vector<uint8_t> key(16, 0x77);
    uint8_t nonce[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    uint8_t aad[52] = {};
    aad[0] = 0xFD;
    aad[1] = 'S';
    aad[2] = 'M';
    aad[3] = 'B';

    std::vector<uint8_t> plaintext(256);
    for (size_t i = 0; i < plaintext.size(); ++i) {
        plaintext[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto ciphertext = crypto.encrypt(key, nonce, 16, aad, 52,
                                     plaintext.data(), plaintext.size());
    TEST_ASSERT(!ciphertext.empty(), "Encrypt should produce ciphertext");

    std::vector<uint8_t> decrypted(plaintext.size());
    size_t dec_len = decrypted.size();
    bool ok = crypto.decrypt(key, nonce, 16, aad, 52,
                             ciphertext.data(), ciphertext.size(),
                             decrypted.data(), dec_len);
    TEST_ASSERT(ok, "Decrypt should succeed");
    decrypted.resize(dec_len);
    TEST_ASSERT(decrypted == plaintext, "Decrypted data should match original");

    return true;
}

bool test_key_derivation()
{
    std::vector<uint8_t> session_key(16, 0x5A);
    std::vector<uint8_t> preauth_hash(64, 0x3C);

    auto signing_key = SmbKeyDerivation::derive_signing_key(
        session_key, DialectRevision::SMB_3_1_1, preauth_hash);
    TEST_ASSERT(!signing_key.empty(), "Should derive signing key");

    auto enc_key = SmbKeyDerivation::derive_encryption_key(
        session_key, DialectRevision::SMB_3_1_1, preauth_hash);
    TEST_ASSERT(!enc_key.empty(), "Should derive encryption key");

    auto dec_key = SmbKeyDerivation::derive_decryption_key(
        session_key, DialectRevision::SMB_3_1_1, preauth_hash);
    TEST_ASSERT(!dec_key.empty(), "Should derive decryption key");

    TEST_ASSERT(enc_key != dec_key, "Encryption and decryption keys should differ");

    auto signing_key_30 = SmbKeyDerivation::derive_signing_key(
        session_key, DialectRevision::SMB_3_0, {});
    TEST_ASSERT(!signing_key_30.empty(), "Should derive SMB3.0 signing key");

    std::vector<uint8_t> fake_preauth_hash(64, 0xA5);
    auto signing_key_30_with_preauth = SmbKeyDerivation::derive_signing_key(
        session_key, DialectRevision::SMB_3_0, fake_preauth_hash);
    TEST_ASSERT(signing_key_30_with_preauth == signing_key_30,
                "SMB3.0 signing key derivation should ignore preauth hash and use SmbSign context");

    auto signing_key_21 = SmbKeyDerivation::derive_signing_key(
        session_key, DialectRevision::SMB_2_1, {});
    TEST_ASSERT(signing_key_21 == session_key, "SMB2.1 signing should use the session key directly");

    TEST_ASSERT(signing_key_30 != signing_key, "Different dialects should produce different keys");

    return true;
}

bool test_server_creation()
{
    SmbServerConfig config;
    config.port = 4445;
    config.server_name = "TEST-SMB";

    SmbServer server(config);
    TEST_ASSERT(server.config().port == 4445, "Port should match config");
    TEST_ASSERT(server.config().server_name == "TEST-SMB", "Server name should match");

    return true;
}

bool test_server_share_init()
{
    SmbServerConfig config;
    config.port = 4445;

    SmbShareConfig share_cfg;
    share_cfg.name = "testshare";
    share_cfg.type = ShareType::DISK;
    share_cfg.path = "/tmp/smb_test_share_init";
    share_cfg.comment = "Test";
    config.shares.push_back(share_cfg);

    SmbServer server(config);

    SmbShare *share = server.share_manager().find_share("testshare");
    TEST_ASSERT(share != nullptr, "Should find configured share");

    SmbShare *ipc = server.share_manager().find_share("IPC$");
    TEST_ASSERT(ipc != nullptr, "Should auto-create IPC$ share");

    return true;
}

bool test_local_file_system()
{
    std::string test_dir = "/tmp/smb_fs_test_" + std::to_string(std::hash<std::thread::id> {}(std::this_thread::get_id()));
    std::filesystem::create_directories(test_dir);

    {
        std::ofstream ofs(test_dir + "/testfile.txt");
        ofs << "Hello SMB World!" << std::endl;
    }

    LocalFileSystem fs(test_dir);

    auto open_result = fs.open("testfile.txt", SMB_FILE_GENERIC_READ, SMB_FILE_OPEN, 0);
    TEST_ASSERT(open_result.success, "Open should succeed");
    TEST_ASSERT(open_result.handle != nullptr, "Should have valid handle");
    TEST_ASSERT(!open_result.is_directory, "Should not be directory");

    auto read_result = fs.read(open_result.handle, 0, 1024);
    TEST_ASSERT(read_result.success, "Read should succeed");
    TEST_ASSERT(read_result.bytes_read > 0, "Should read some bytes");

    std::string content(read_result.data.begin(), read_result.data.end());
    TEST_ASSERT(content.find("Hello SMB World!") != std::string::npos,
                "Read content should match written content");

    auto info = fs.query_info(open_result.handle, FileInfoClass::FileBasicInformation);
    TEST_ASSERT(info.has_value(), "Query info should succeed");

    fs.close(open_result.handle);
    std::filesystem::remove_all(test_dir);
    return true;
}

bool test_local_file_system_directory()
{
    std::string test_dir = "/tmp/smb_fs_dir_test_" + std::to_string(std::hash<std::thread::id> {}(std::this_thread::get_id()));
    std::filesystem::create_directories(test_dir);
    std::filesystem::create_directories(test_dir + "/subdir1");
    std::filesystem::create_directories(test_dir + "/subdir2");

    {
        std::ofstream ofs(test_dir + "/file1.txt");
        ofs << "file1" << std::endl;
    }
    {
        std::ofstream ofs(test_dir + "/file2.txt");
        ofs << "file2" << std::endl;
    }

    LocalFileSystem fs(test_dir);

    auto open_result = fs.open("", SMB_FILE_GENERIC_READ, SMB_FILE_OPEN, SMB_FILE_DIRECTORY_FILE);
    TEST_ASSERT(open_result.success, "Open directory should succeed");
    TEST_ASSERT(open_result.is_directory, "Should be directory");

    auto entries = fs.query_directory(open_result.handle, "*", FileInfoClass::FileIdBothDirectoryInformation, true);
    TEST_ASSERT(entries.has_value(), "Query directory should succeed");
    TEST_ASSERT(entries->size() >= 2, "Should have at least 2 entries");

    bool found_file1 = false;
    for (const auto &e : *entries) {
        std::string name = Smb2Codec::utf16le_to_utf8(e.file_name);
        if (name == "file1.txt")
            found_file1 = true;
    }
    TEST_ASSERT(found_file1, "Should find file1.txt in directory listing");

    fs.close(open_result.handle);
    std::filesystem::remove_all(test_dir);
    return true;
}

bool test_local_file_system_set_info_rename_delete()
{
    std::string test_dir = "/tmp/smb_fs_mutation_test_" + std::to_string(std::hash<std::thread::id> {}(std::this_thread::get_id()));
    std::filesystem::create_directories(test_dir);

    {
        std::ofstream ofs(test_dir + "/old.txt");
        ofs << "rename me" << std::endl;
    }

    LocalFileSystem fs(test_dir);

    auto open_result = fs.open("old.txt", SMB_FILE_GENERIC_READ | SMB_FILE_GENERIC_WRITE, SMB_FILE_OPEN, 0);
    TEST_ASSERT(open_result.success, "Open file for mutation should succeed");

    auto new_name = Smb2Codec::utf8_to_utf16le("new.txt");
    std::vector<uint8_t> rename_info(20 + new_name.size() * 2, 0);
    rename_info[0] = 1;
    uint32_t name_len = static_cast<uint32_t>(new_name.size() * 2);
    std::memcpy(rename_info.data() + 16, &name_len, sizeof(name_len));
    for (size_t i = 0; i < new_name.size(); ++i) {
        rename_info[20 + i * 2] = static_cast<uint8_t>(new_name[i] & 0xFF);
        rename_info[21 + i * 2] = static_cast<uint8_t>((new_name[i] >> 8) & 0xFF);
    }

    auto rename_status = fs.set_info(open_result.handle, FileInfoClass::FileRenameInformation,
                                     rename_info.data(), static_cast<uint32_t>(rename_info.size()));
    TEST_ASSERT(rename_status == NtStatus::SUCCESS, "Rename via FileRenameInformation should succeed");
    TEST_ASSERT(std::filesystem::exists(test_dir + "/new.txt"), "Renamed file should exist");
    TEST_ASSERT(!std::filesystem::exists(test_dir + "/old.txt"), "Old file name should be gone");

    uint8_t delete_info[1] = { 1 };
    auto delete_status = fs.set_info(open_result.handle, FileInfoClass::FileDispositionInformation,
                                     delete_info, sizeof(delete_info));
    TEST_ASSERT(delete_status == NtStatus::SUCCESS, "Delete via FileDispositionInformation should succeed");
    TEST_ASSERT(std::filesystem::exists(test_dir + "/new.txt"), "Delete-on-close should defer removal until close");

    fs.close(open_result.handle);
    TEST_ASSERT(!std::filesystem::exists(test_dir + "/new.txt"), "Deleted file should be gone after close");
    std::filesystem::remove_all(test_dir);
    return true;
}

bool test_local_file_system_absolute_rename()
{
    std::string test_dir = "/tmp/smb_fs_abs_rename_test_" + std::to_string(std::hash<std::thread::id> {}(std::this_thread::get_id()));
    std::filesystem::create_directories(test_dir);

    {
        std::ofstream ofs(test_dir + "/old.txt");
        ofs << "rename me from root" << std::endl;
    }

    LocalFileSystem fs(test_dir);

    auto open_result = fs.open("old.txt", SMB_FILE_GENERIC_READ | SMB_FILE_GENERIC_WRITE, SMB_FILE_OPEN, 0);
    TEST_ASSERT(open_result.success, "Open file for absolute rename should succeed");

    auto new_name = Smb2Codec::utf8_to_utf16le("\\new.txt");
    std::vector<uint8_t> rename_info(20 + new_name.size() * 2, 0);
    rename_info[0] = 1;
    uint32_t name_len = static_cast<uint32_t>(new_name.size() * 2);
    std::memcpy(rename_info.data() + 16, &name_len, sizeof(name_len));
    for (size_t i = 0; i < new_name.size(); ++i) {
        rename_info[20 + i * 2] = static_cast<uint8_t>(new_name[i] & 0xFF);
        rename_info[21 + i * 2] = static_cast<uint8_t>((new_name[i] >> 8) & 0xFF);
    }

    auto rename_status = fs.set_info(open_result.handle, FileInfoClass::FileRenameInformation,
                                     rename_info.data(), static_cast<uint32_t>(rename_info.size()));
    TEST_ASSERT(rename_status == NtStatus::SUCCESS, "Root-relative SMB rename should succeed");
    TEST_ASSERT(std::filesystem::exists(test_dir + "/new.txt"), "Root-relative renamed file should exist in share");
    TEST_ASSERT(!std::filesystem::exists(test_dir + "/old.txt"), "Root-relative old file should be gone");

    fs.close(open_result.handle);
    std::filesystem::remove_all(test_dir);
    return true;
}

bool test_local_file_system_directory_cursor()
{
    std::string test_dir = "/tmp/smb_fs_cursor_test_" + std::to_string(std::hash<std::thread::id> {}(std::this_thread::get_id()));
    std::filesystem::create_directories(test_dir);

    {
        std::ofstream ofs(test_dir + "/a.txt");
        ofs << "a" << std::endl;
    }
    {
        std::ofstream ofs(test_dir + "/b.txt");
        ofs << "b" << std::endl;
    }

    LocalFileSystem fs(test_dir);

    auto open_result = fs.open("", SMB_FILE_GENERIC_READ, SMB_FILE_OPEN, SMB_FILE_DIRECTORY_FILE);
    TEST_ASSERT(open_result.success, "Open directory for cursor test should succeed");

    auto first = fs.query_directory(open_result.handle, "*.txt", FileInfoClass::FileIdBothDirectoryInformation, true, 1);
    TEST_ASSERT(first.has_value(), "First paged query should return an entry");
    TEST_ASSERT(first->size() == 1, "First paged query should return one entry");

    auto second = fs.query_directory(open_result.handle, "*.txt", FileInfoClass::FileIdBothDirectoryInformation, false, 1);
    TEST_ASSERT(second.has_value(), "Second paged query should return an entry");
    TEST_ASSERT(second->size() == 1, "Second paged query should return one entry");

    auto done = fs.query_directory(open_result.handle, "*.txt", FileInfoClass::FileIdBothDirectoryInformation, false, 1);
    TEST_ASSERT(!done.has_value(), "Paged directory query should end with no more files");

    auto restarted = fs.query_directory(open_result.handle, "*.txt", FileInfoClass::FileIdBothDirectoryInformation, true, 1);
    TEST_ASSERT(restarted.has_value(), "Restarted directory query should return entries again");

    fs.close(open_result.handle);
    std::filesystem::remove_all(test_dir);
    return true;
}

bool test_dispatcher_negotiate()
{
    SmbServerConfig config;
    config.port = 4445;
    SmbShareManager share_mgr;
    SmbLockManager lock_mgr;
    SmbPipeManager pipe_mgr;
    SmbDfsResolver dfs;
    SmbChangeNotifier cn;

    SmbDispatcher disp(config, share_mgr, lock_mgr, pipe_mgr, dfs, cn);

    SmbSessionManager session_mgr;
    SmbSession *session = session_mgr.create_session(nullptr);

    Smb2Header hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.protocol_id = SMB2_PROTOCOL_ID;
    hdr.command = static_cast<uint16_t>(Smb2Command::NEGOTIATE);
    hdr.credit_request = 1;

    Smb2NegotiateRequest req;
    req.dialect_count = 2;
    req.dialects = {
        static_cast<uint16_t>(DialectRevision::SMB_2_002),
        static_cast<uint16_t>(DialectRevision::SMB_3_0)
    };

    ByteBuffer req_buf = Smb2Codec::encode_header(hdr);
    Smb2Codec::write_le16(req_buf, 36);
    Smb2Codec::write_le16(req_buf, req.dialect_count);
    Smb2Codec::write_le16(req_buf, 0);
    Smb2Codec::write_le16(req_buf, 0);
    for (auto d : req.dialects) {
        Smb2Codec::write_le16(req_buf, d);
    }

    auto span = req_buf.readable_span();
    auto resp = disp.dispatch(*session, hdr,
                              reinterpret_cast<const uint8_t *>(span.data()),
                              span.size());

    TEST_ASSERT(resp.readable_bytes() >= SMB2_HEADER_SIZE, "Negotiate response should have header");

    auto resp_span = resp.readable_span();
    auto resp_hdr = Smb2Codec::decode_header(
        reinterpret_cast<const uint8_t *>(resp_span.data()), resp_span.size());
    TEST_ASSERT(resp_hdr.has_value(), "Should decode negotiate response header");
    TEST_ASSERT(resp_hdr->status == 0, "Negotiate should succeed");

    session_mgr.remove_session(session->session_id());
    return true;
}

bool test_dispatcher_negotiate_conservative_capabilities()
{
    SmbServerConfig config;
    config.enable_encryption = false;
    SmbShareManager share_mgr;
    SmbLockManager lock_mgr;
    SmbPipeManager pipe_mgr;
    SmbDfsResolver dfs;
    SmbChangeNotifier cn;

    SmbDispatcher disp(config, share_mgr, lock_mgr, pipe_mgr, dfs, cn);

    SmbSessionManager session_mgr;
    SmbSession *session = session_mgr.create_session(nullptr);

    Smb2Header hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.protocol_id = SMB2_PROTOCOL_ID;
    hdr.command = static_cast<uint16_t>(Smb2Command::NEGOTIATE);
    hdr.credit_request = 1;

    ByteBuffer req_buf = Smb2Codec::encode_header(hdr);
    Smb2Codec::write_le16(req_buf, 36);
    Smb2Codec::write_le16(req_buf, 1);
    Smb2Codec::write_le16(req_buf, 0);
    Smb2Codec::write_le16(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    for (int i = 0; i < 16; ++i) {
        req_buf.append_u8(0);
    }
    Smb2Codec::write_le64(req_buf, 0);
    Smb2Codec::write_le16(req_buf, static_cast<uint16_t>(DialectRevision::SMB_3_1_1));

    auto span = req_buf.readable_span();
    auto resp = disp.dispatch(*session, hdr,
                              reinterpret_cast<const uint8_t *>(span.data()),
                              span.size());
    auto resp_span = resp.readable_span();
    const auto *raw = reinterpret_cast<const uint8_t *>(resp_span.data());
    auto resp_hdr = Smb2Codec::decode_header(raw, resp_span.size());
    TEST_ASSERT(resp_hdr.has_value(), "Should decode conservative negotiate response");
    TEST_ASSERT(resp_hdr->status == 0, "Conservative negotiate should succeed");

    const uint32_t caps = Smb2Codec::read_le32(raw + SMB2_HEADER_SIZE + 24);
    TEST_ASSERT((caps & SMB2_GLOBAL_CAP_ENCRYPTION) == 0, "Encryption should not be advertised when disabled");
    TEST_ASSERT((caps & SMB2_GLOBAL_CAP_MULTI_CHANNEL) == 0, "Multi-channel should not be advertised before support");
    TEST_ASSERT((caps & SMB2_GLOBAL_CAP_PERSISTENT_HANDLES) == 0, "Persistent handles should not be advertised before support");
    TEST_ASSERT((caps & SMB2_GLOBAL_CAP_DIRECTORY_LEASING) == 0, "Directory leasing should not be advertised before support");
    TEST_ASSERT((caps & SMB2_GLOBAL_CAP_LARGE_MTU) != 0, "Large MTU should still be advertised");

    session_mgr.remove_session(session->session_id());
    return true;
}

bool test_dispatcher_negotiate_signing_required_mode()
{
    SmbServerConfig config;
    config.require_signing = true;
    SmbShareManager share_mgr;
    SmbLockManager lock_mgr;
    SmbPipeManager pipe_mgr;
    SmbDfsResolver dfs;
    SmbChangeNotifier cn;

    SmbDispatcher disp(config, share_mgr, lock_mgr, pipe_mgr, dfs, cn);

    SmbSessionManager session_mgr;
    SmbSession *session = session_mgr.create_session(nullptr);

    Smb2Header hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.protocol_id = SMB2_PROTOCOL_ID;
    hdr.command = static_cast<uint16_t>(Smb2Command::NEGOTIATE);
    hdr.credit_request = 1;

    ByteBuffer req_buf = Smb2Codec::encode_header(hdr);
    Smb2Codec::write_le16(req_buf, 36);
    Smb2Codec::write_le16(req_buf, 1);
    Smb2Codec::write_le16(req_buf, 0);
    Smb2Codec::write_le16(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    for (int i = 0; i < 16; ++i) {
        req_buf.append_u8(0);
    }
    Smb2Codec::write_le64(req_buf, 0);
    Smb2Codec::write_le16(req_buf, static_cast<uint16_t>(DialectRevision::SMB_3_1_1));

    auto span = req_buf.readable_span();
    auto resp = disp.dispatch(*session, hdr,
                              reinterpret_cast<const uint8_t *>(span.data()),
                              span.size());
    auto resp_span = resp.readable_span();
    const auto *raw = reinterpret_cast<const uint8_t *>(resp_span.data());
    auto resp_hdr = Smb2Codec::decode_header(raw, resp_span.size());
    TEST_ASSERT(resp_hdr.has_value(), "Should decode signing-required negotiate response");
    TEST_ASSERT(resp_hdr->status == 0, "Signing-required negotiate should succeed");

    const uint16_t security_mode = Smb2Codec::read_le16(raw + SMB2_HEADER_SIZE + 2);
    const auto required_flag = static_cast<uint16_t>(SecurityMode::NEGOTIATE_SIGNING_REQUIRED);
    TEST_ASSERT((security_mode & required_flag) != 0,
                "Negotiate response should advertise signing required when enabled");

    session_mgr.remove_session(session->session_id());
    return true;
}

bool test_negotiate_response_layout_for_smb311()
{
    Smb2Header hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.protocol_id = SMB2_PROTOCOL_ID;
    hdr.command = static_cast<uint16_t>(Smb2Command::NEGOTIATE);
    hdr.message_id = 7;

    Smb2NegotiateResponse resp;
    resp.dialect_revision = static_cast<uint16_t>(DialectRevision::SMB_3_1_1);
    resp.security_mode = static_cast<uint16_t>(SecurityMode::NEGOTIATE_SIGNING_ENABLED);
    resp.capabilities = SMB2_GLOBAL_CAP_LARGE_MTU;
    resp.max_transact_size = 65536;
    resp.max_read_size = 65536;
    resp.max_write_size = 65536;
    resp.system_time = 1;
    resp.server_start_time = 1;
    resp.security_buffer = {0x60, 0x03, 0x06, 0x01};

    ByteBuffer preauth_buf(64);
    Smb2Codec::write_le16(preauth_buf, 1);
    Smb2Codec::write_le16(preauth_buf, 32);
    Smb2Codec::write_le16(preauth_buf, static_cast<uint16_t>(SMB2_PREAUTH_INTEGRITY_CAP_SHA_512));
    for (int i = 0; i < 32; ++i) {
        preauth_buf.append_u8(static_cast<uint8_t>(i + 1));
    }
    NegotiateContext preauth;
    preauth.context_type = 0x0001;
    auto preauth_span = preauth_buf.readable_span();
    preauth.data.assign(reinterpret_cast<const uint8_t *>(preauth_span.data()),
                        reinterpret_cast<const uint8_t *>(preauth_span.data()) + preauth_span.size());

    ByteBuffer encryption_buf(16);
    Smb2Codec::write_le16(encryption_buf, 2);
    Smb2Codec::write_le16(encryption_buf, static_cast<uint16_t>(EncryptionAlgorithm::AES_128_GCM));
    Smb2Codec::write_le16(encryption_buf, static_cast<uint16_t>(EncryptionAlgorithm::AES_128_CCM));
    NegotiateContext encryption;
    encryption.context_type = 0x0002;
    auto encryption_span = encryption_buf.readable_span();
    encryption.data.assign(reinterpret_cast<const uint8_t *>(encryption_span.data()),
                           reinterpret_cast<const uint8_t *>(encryption_span.data()) + encryption_span.size());

    auto ctx_buf = Smb2Codec::encode_negotiate_contexts({preauth, encryption});
    auto ctx_span = ctx_buf.readable_span();
    resp.negotiate_context.assign(reinterpret_cast<const uint8_t *>(ctx_span.data()),
                                  reinterpret_cast<const uint8_t *>(ctx_span.data()) + ctx_span.size());
    resp.negotiate_context_count = 2;

    auto encoded = Smb2Codec::encode_negotiate_response(hdr, resp);
    auto span = encoded.readable_span();
    const auto *raw = reinterpret_cast<const uint8_t *>(span.data());

    TEST_ASSERT(span.size() >= SMB2_HEADER_SIZE + 64, "SMB3.1.1 negotiate response should include fixed body");
    TEST_ASSERT(Smb2Codec::read_le16(raw + SMB2_HEADER_SIZE + 4) ==
                    static_cast<uint16_t>(DialectRevision::SMB_3_1_1),
                "Dialect should be encoded at negotiate response offset 4");
    TEST_ASSERT(Smb2Codec::read_le16(raw + SMB2_HEADER_SIZE + 6) == 2,
                "Negotiate context count should occupy SMB3.1.1 reserved field");
    TEST_ASSERT(Smb2Codec::read_le16(raw + SMB2_HEADER_SIZE + 56) == SMB2_HEADER_SIZE + 64,
                "Security buffer offset should point immediately after fixed body");
    TEST_ASSERT(Smb2Codec::read_le16(raw + SMB2_HEADER_SIZE + 58) == resp.security_buffer.size(),
                "Security buffer length should match encoded token size");

    const uint32_t ctx_offset = Smb2Codec::read_le32(raw + SMB2_HEADER_SIZE + 60);
    const uint32_t expected_ctx_offset = (SMB2_HEADER_SIZE + 64 + resp.security_buffer.size() + 7u) & ~7u;
    TEST_ASSERT(ctx_offset == expected_ctx_offset,
                "Negotiate context offset should be aligned after security buffer");
    TEST_ASSERT(ctx_offset + resp.negotiate_context.size() <= span.size(),
                "Negotiate contexts should fit within encoded response");

    return true;
}

bool test_dispatcher_query_filesystem_info()
{
    std::string test_dir = "/tmp/smb_fs_info_test_" + std::to_string(std::hash<std::thread::id> {}(std::this_thread::get_id()));
    std::filesystem::create_directories(test_dir);

    SmbServerConfig config;
    SmbShareManager share_mgr;
    SmbShareConfig share_cfg;
    share_cfg.name = "public";
    share_cfg.type = ShareType::DISK;
    share_cfg.path = test_dir;
    share_mgr.add_share(share_cfg);

    auto fs = std::make_unique<LocalFileSystem>(test_dir);
    LocalFileSystem *fs_ptr = fs.get();
    SmbShare *share = share_mgr.find_share("public");
    TEST_ASSERT(share != nullptr, "Filesystem info share should exist");
    share->set_file_system(std::move(fs));

    SmbLockManager lock_mgr;
    SmbPipeManager pipe_mgr;
    SmbDfsResolver dfs;
    SmbChangeNotifier cn;
    SmbDispatcher disp(config, share_mgr, lock_mgr, pipe_mgr, dfs, cn);

    SmbSessionManager session_mgr;
    SmbSession *session = session_mgr.create_session(nullptr);
    session->set_state(SmbSession::State::active);

    TreeConnection tree;
    tree.share_name = "public";
    tree.share = share;
    const uint32_t tree_id = session->add_tree_connection(std::move(tree));

    auto open_result = fs_ptr->open("", SMB_FILE_GENERIC_READ, SMB_FILE_OPEN, SMB_FILE_DIRECTORY_FILE);
    TEST_ASSERT(open_result.success, "Open share root for filesystem info should succeed");

    FileId fid = session->allocate_file_id();
    yuan::net::smb::OpenFile of;
    of.file_id = fid;
    of.path = test_dir;
    of.is_directory = true;
    of.file_handle = open_result.handle;
    of.tree_id = tree_id;
    share->add_open_file(fid, std::move(of));

    Smb2Header hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.protocol_id = SMB2_PROTOCOL_ID;
    hdr.command = static_cast<uint16_t>(Smb2Command::QUERY_INFO);
    hdr.tree_id = tree_id;
    hdr.session_id = session->session_id();
    hdr.credit_request = 1;

    ByteBuffer req_buf = Smb2Codec::encode_header(hdr);
    Smb2Codec::write_le16(req_buf, 41);
    req_buf.append_u8(SMB2_0_INFO_FILESYSTEM);
    req_buf.append_u8(static_cast<uint8_t>(FileInfoClass::FileFsAttributeInformation));
    Smb2Codec::write_le32(req_buf, 1024);
    Smb2Codec::write_le16(req_buf, 0);
    Smb2Codec::write_le16(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    Smb2Codec::write_le64(req_buf, fid.persistent);
    Smb2Codec::write_le64(req_buf, fid.volatile_id);
    req_buf.append_u8(0);

    auto span = req_buf.readable_span();
    auto resp = disp.dispatch(*session, hdr,
                              reinterpret_cast<const uint8_t *>(span.data()),
                              span.size());
    auto resp_span = resp.readable_span();
    const auto *raw = reinterpret_cast<const uint8_t *>(resp_span.data());
    auto resp_hdr = Smb2Codec::decode_header(raw, resp_span.size());
    TEST_ASSERT(resp_hdr.has_value(), "Should decode filesystem info response");
    TEST_ASSERT(resp_hdr->status == 0, "Filesystem info should succeed");

    const uint32_t output_len = Smb2Codec::read_le32(raw + SMB2_HEADER_SIZE + 4);
    TEST_ASSERT(output_len >= 20, "Filesystem attribute info should include payload");
    const uint32_t fs_attrs = Smb2Codec::read_le32(raw + SMB2_HEADER_SIZE + 8);
    TEST_ASSERT((fs_attrs & SMB_FILE_UNICODE_ON_DISK) != 0, "Filesystem attrs should advertise unicode names");

    share->remove_open_file(fid);
    fs_ptr->close(open_result.handle);
    session_mgr.remove_session(session->session_id());
    std::filesystem::remove_all(test_dir);
    return true;
}

bool test_dispatcher_ioctl_validate_negotiate_info()
{
    SmbServerConfig config;
    config.enable_encryption = true;
    SmbShareManager share_mgr;
    SmbLockManager lock_mgr;
    SmbPipeManager pipe_mgr;
    SmbDfsResolver dfs;
    SmbChangeNotifier cn;
    SmbDispatcher disp(config, share_mgr, lock_mgr, pipe_mgr, dfs, cn);

    SmbSessionManager session_mgr;
    SmbSession *session = session_mgr.create_session(nullptr);
    session->set_dialect(DialectRevision::SMB_3_1_1);
    session->set_server_capabilities(SMB2_GLOBAL_CAP_LEASING | SMB2_GLOBAL_CAP_LARGE_MTU | SMB2_GLOBAL_CAP_ENCRYPTION);
    session->set_server_security_mode(static_cast<uint16_t>(SecurityMode::NEGOTIATE_SIGNING_ENABLED));

    Smb2Header hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.protocol_id = SMB2_PROTOCOL_ID;
    hdr.command = static_cast<uint16_t>(Smb2Command::IOCTL);
    hdr.session_id = session->session_id();
    hdr.credit_request = 1;

    std::vector<uint8_t> input;
    input.resize(24 + 2, 0);
    test_write_le32(input, 0, session->server_capabilities());
    test_write_le16(input, 20, session->server_security_mode());
    test_write_le16(input, 22, 1);
    test_write_le16(input, 24, static_cast<uint16_t>(DialectRevision::SMB_3_1_1));

    ByteBuffer req_buf = Smb2Codec::encode_header(hdr);
    Smb2Codec::write_le16(req_buf, 57);
    Smb2Codec::write_le16(req_buf, 0);
    Smb2Codec::write_le32(req_buf, FSCTL_VALIDATE_NEGOTIATE_INFO);
    Smb2Codec::write_le64(req_buf, 0);
    Smb2Codec::write_le64(req_buf, 0);
    Smb2Codec::write_le32(req_buf, SMB2_HEADER_SIZE + 56);
    Smb2Codec::write_le32(req_buf, static_cast<uint32_t>(input.size()));
    Smb2Codec::write_le32(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 1024);
    Smb2Codec::write_le32(req_buf, 1);
    Smb2Codec::write_le32(req_buf, 0);
    req_buf.append(input.data(), input.size());

    auto span = req_buf.readable_span();
    auto resp = disp.dispatch(*session, hdr,
                              reinterpret_cast<const uint8_t *>(span.data()),
                              span.size());
    auto resp_span = resp.readable_span();
    const auto *raw = reinterpret_cast<const uint8_t *>(resp_span.data());
    auto resp_hdr = Smb2Codec::decode_header(raw, resp_span.size());
    TEST_ASSERT(resp_hdr.has_value(), "Should decode validate negotiate response");
    TEST_ASSERT(resp_hdr->status == 0, "Validate negotiate should succeed");
    TEST_ASSERT(Smb2Codec::read_le32(raw + SMB2_HEADER_SIZE + 4) == FSCTL_VALIDATE_NEGOTIATE_INFO,
                "Validate negotiate ctl code should echo");
    TEST_ASSERT(Smb2Codec::read_le32(raw + SMB2_HEADER_SIZE + 36) == 24,
                "Validate negotiate output should be 24 bytes");
    TEST_ASSERT(Smb2Codec::read_le16(raw + SMB2_HEADER_SIZE + 48 + 22) ==
                    static_cast<uint16_t>(DialectRevision::SMB_3_1_1),
                "Validate negotiate output dialect should match session");

    session_mgr.remove_session(session->session_id());
    return true;
}

bool test_dispatcher_ioctl_query_network_interface_info()
{
    SmbServerConfig config;
    SmbShareManager share_mgr;
    SmbLockManager lock_mgr;
    SmbPipeManager pipe_mgr;
    SmbDfsResolver dfs;
    SmbChangeNotifier cn;
    SmbDispatcher disp(config, share_mgr, lock_mgr, pipe_mgr, dfs, cn);

    SmbSessionManager session_mgr;
    SmbSession *session = session_mgr.create_session(nullptr);

    Smb2Header hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.protocol_id = SMB2_PROTOCOL_ID;
    hdr.command = static_cast<uint16_t>(Smb2Command::IOCTL);
    hdr.session_id = session->session_id();
    hdr.credit_request = 1;

    ByteBuffer req_buf = Smb2Codec::encode_header(hdr);
    Smb2Codec::write_le16(req_buf, 57);
    Smb2Codec::write_le16(req_buf, 0);
    Smb2Codec::write_le32(req_buf, FSCTL_QUERY_NETWORK_INTERFACE_INFO);
    Smb2Codec::write_le64(req_buf, 0);
    Smb2Codec::write_le64(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 1024);
    Smb2Codec::write_le32(req_buf, 1);
    Smb2Codec::write_le32(req_buf, 0);
    req_buf.append_u8(0);

    auto span = req_buf.readable_span();
    auto resp = disp.dispatch(*session, hdr,
                              reinterpret_cast<const uint8_t *>(span.data()),
                              span.size());
    auto resp_span = resp.readable_span();
    const auto *raw = reinterpret_cast<const uint8_t *>(resp_span.data());
    auto resp_hdr = Smb2Codec::decode_header(raw, resp_span.size());
    TEST_ASSERT(resp_hdr.has_value(), "Should decode network interface response");
    TEST_ASSERT(resp_hdr->status == 0, "Query network interface should succeed");
    TEST_ASSERT(Smb2Codec::read_le32(raw + SMB2_HEADER_SIZE + 36) == 152,
                "Network interface response should contain one 152-byte entry");
    TEST_ASSERT(Smb2Codec::read_le32(raw + SMB2_HEADER_SIZE + 48 + 8) == SMB2_NETWORK_INTERFACE_RSS_CAPABLE,
                "Network interface should advertise RSS capability");
    TEST_ASSERT(Smb2Codec::read_le16(raw + SMB2_HEADER_SIZE + 48 + 24) == 2,
                "Network interface sockaddr should be IPv4");

    session_mgr.remove_session(session->session_id());
    return true;
}

bool test_dispatcher_ioctl_permission_denied()
{
    class DenyIoctlHandler final : public SmbHandler
    {
    public:
        bool called = false;
        uint32_t last_ctl_code = 0;

        bool on_ioctl(SmbSession *, const FileId &, uint32_t ctl_code) override
        {
            called = true;
            last_ctl_code = ctl_code;
            return false;
        }
    };

    SmbServerConfig config;
    SmbShareManager share_mgr;
    SmbLockManager lock_mgr;
    SmbPipeManager pipe_mgr;
    SmbDfsResolver dfs;
    SmbChangeNotifier cn;
    DenyIoctlHandler handler;
    SmbDispatcher disp(config, share_mgr, lock_mgr, pipe_mgr, dfs, cn, &handler);

    SmbSessionManager session_mgr;
    SmbSession *session = session_mgr.create_session(nullptr);

    Smb2Header hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.protocol_id = SMB2_PROTOCOL_ID;
    hdr.command = static_cast<uint16_t>(Smb2Command::IOCTL);
    hdr.session_id = session->session_id();
    hdr.credit_request = 1;

    ByteBuffer req_buf = Smb2Codec::encode_header(hdr);
    Smb2Codec::write_le16(req_buf, 57);
    Smb2Codec::write_le16(req_buf, 0);
    Smb2Codec::write_le32(req_buf, FSCTL_QUERY_NETWORK_INTERFACE_INFO);
    Smb2Codec::write_le64(req_buf, 0);
    Smb2Codec::write_le64(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 1024);
    Smb2Codec::write_le32(req_buf, 1);
    Smb2Codec::write_le32(req_buf, 0);
    req_buf.append_u8(0);

    auto span = req_buf.readable_span();
    auto resp = disp.dispatch(*session, hdr,
                              reinterpret_cast<const uint8_t *>(span.data()),
                              span.size());
    auto resp_span = resp.readable_span();
    const auto *raw = reinterpret_cast<const uint8_t *>(resp_span.data());
    auto resp_hdr = Smb2Codec::decode_header(raw, resp_span.size());
    TEST_ASSERT(resp_hdr.has_value(), "Should decode denied ioctl response");
    TEST_ASSERT(resp_hdr->status == static_cast<uint32_t>(NtStatus::ACCESS_DENIED),
                "Denied ioctl should map to ACCESS_DENIED");
    TEST_ASSERT(handler.called, "Denied ioctl should invoke handler hook");
    TEST_ASSERT(handler.last_ctl_code == FSCTL_QUERY_NETWORK_INTERFACE_INFO,
                "Denied ioctl should forward ctl code to handler");

    session_mgr.remove_session(session->session_id());
    return true;
}

bool test_dispatcher_change_notify_completes_empty()
{
    SmbServerConfig config;
    SmbShareManager share_mgr;
    SmbLockManager lock_mgr;
    SmbPipeManager pipe_mgr;
    SmbDfsResolver dfs;
    SmbChangeNotifier cn;
    SmbDispatcher disp(config, share_mgr, lock_mgr, pipe_mgr, dfs, cn);

    SmbSessionManager session_mgr;
    SmbSession *session = session_mgr.create_session(nullptr);
    session->set_state(SmbSession::State::active);

    FileId fid;
    fid.persistent = 10;
    fid.volatile_id = 10;

    Smb2Header hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.protocol_id = SMB2_PROTOCOL_ID;
    hdr.command = static_cast<uint16_t>(Smb2Command::CHANGE_NOTIFY);
    hdr.session_id = session->session_id();
    hdr.credit_request = 1;

    ByteBuffer req_buf = Smb2Codec::encode_header(hdr);
    Smb2Codec::write_le16(req_buf, 32);
    Smb2Codec::write_le16(req_buf, 0);
    Smb2Codec::write_le32(req_buf, 1024);
    Smb2Codec::write_le64(req_buf, fid.persistent);
    Smb2Codec::write_le64(req_buf, fid.volatile_id);
    Smb2Codec::write_le32(req_buf, 0x00000003);
    Smb2Codec::write_le32(req_buf, 0);

    auto span = req_buf.readable_span();
    auto resp = disp.dispatch(*session, hdr,
                              reinterpret_cast<const uint8_t *>(span.data()),
                              span.size());
    auto resp_span = resp.readable_span();
    const auto *raw = reinterpret_cast<const uint8_t *>(resp_span.data());
    auto resp_hdr = Smb2Codec::decode_header(raw, resp_span.size());
    TEST_ASSERT(resp_hdr.has_value(), "Should decode change notify response");
    TEST_ASSERT(resp_hdr->status == 0, "Change notify should complete successfully");
    TEST_ASSERT(resp_hdr->command == static_cast<uint16_t>(Smb2Command::CHANGE_NOTIFY),
                "Change notify response command should match");
    TEST_ASSERT(Smb2Codec::read_le32(raw + SMB2_HEADER_SIZE + 4) == 0,
                "Change notify fallback should return empty buffer");

    session_mgr.remove_session(session->session_id());
    return true;
}

bool test_dispatcher_set_info_permission_hooks()
{
    class RecordingHandler final : public SmbHandler
    {
    public:
        bool allow_rename = false;
        bool allow_delete = false;
        bool rename_called = false;
        bool delete_called = false;
        std::string last_rename_path;

        bool on_set_info(SmbSession *, const FileId &) override
        {
            return true;
        }

        bool on_rename(SmbSession *, const FileId &, const std::string &new_path) override
        {
            rename_called = true;
            last_rename_path = new_path;
            return allow_rename;
        }

        bool on_delete(SmbSession *, const FileId &) override
        {
            delete_called = true;
            return allow_delete;
        }
    };

    std::string test_dir = "/tmp/smb_dispatcher_set_info_perm_" + std::to_string(std::hash<std::thread::id> {}(std::this_thread::get_id()));
    std::filesystem::create_directories(test_dir);
    {
        std::ofstream ofs(test_dir + "/old.txt");
        ofs << "dispatcher set info" << std::endl;
    }

    SmbServerConfig config;
    SmbShareManager share_mgr;
    SmbShareConfig share_cfg;
    share_cfg.name = "public";
    share_cfg.type = ShareType::DISK;
    share_cfg.path = test_dir;
    share_mgr.add_share(share_cfg);

    auto fs = std::make_unique<LocalFileSystem>(test_dir);
    LocalFileSystem *fs_ptr = fs.get();
    SmbShare *share = share_mgr.find_share("public");
    TEST_ASSERT(share != nullptr, "SetInfo permission share should exist");
    share->set_file_system(std::move(fs));

    SmbLockManager lock_mgr;
    SmbPipeManager pipe_mgr;
    SmbDfsResolver dfs;
    SmbChangeNotifier cn;
    RecordingHandler handler;
    SmbDispatcher disp(config, share_mgr, lock_mgr, pipe_mgr, dfs, cn, &handler);

    SmbSessionManager session_mgr;
    SmbSession *session = session_mgr.create_session(nullptr);
    session->set_state(SmbSession::State::active);

    TreeConnection tree;
    tree.share_name = "public";
    tree.share = share;
    const uint32_t tree_id = session->add_tree_connection(std::move(tree));

    auto open_result = fs_ptr->open("old.txt", SMB_FILE_GENERIC_READ | SMB_FILE_GENERIC_WRITE, SMB_FILE_OPEN, 0);
    TEST_ASSERT(open_result.success, "Open file for dispatcher set_info permission should succeed");

    FileId fid = session->allocate_file_id();
    yuan::net::smb::OpenFile of;
    of.file_id = fid;
    of.path = test_dir + "/old.txt";
    of.is_directory = false;
    of.file_handle = open_result.handle;
    of.tree_id = tree_id;
    share->add_open_file(fid, std::move(of));

    Smb2Header hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.protocol_id = SMB2_PROTOCOL_ID;
    hdr.command = static_cast<uint16_t>(Smb2Command::SET_INFO);
    hdr.tree_id = tree_id;
    hdr.session_id = session->session_id();
    hdr.credit_request = 1;

    auto new_name = Smb2Codec::utf8_to_utf16le("new.txt");
    std::vector<uint8_t> rename_info(20 + new_name.size() * 2, 0);
    rename_info[0] = 1;
    uint32_t name_len = static_cast<uint32_t>(new_name.size() * 2);
    std::memcpy(rename_info.data() + 16, &name_len, sizeof(name_len));
    for (size_t i = 0; i < new_name.size(); ++i) {
        rename_info[20 + i * 2] = static_cast<uint8_t>(new_name[i] & 0xFF);
        rename_info[21 + i * 2] = static_cast<uint8_t>((new_name[i] >> 8) & 0xFF);
    }

    ByteBuffer rename_req = Smb2Codec::encode_header(hdr);
    Smb2Codec::write_le16(rename_req, 33);
    rename_req.append_u8(SMB2_0_INFO_FILE);
    rename_req.append_u8(static_cast<uint8_t>(FileInfoClass::FileRenameInformation));
    Smb2Codec::write_le32(rename_req, static_cast<uint32_t>(rename_info.size()));
    Smb2Codec::write_le16(rename_req, SMB2_HEADER_SIZE + 32);
    Smb2Codec::write_le16(rename_req, 0);
    Smb2Codec::write_le32(rename_req, 0);
    Smb2Codec::write_le64(rename_req, fid.persistent);
    Smb2Codec::write_le64(rename_req, fid.volatile_id);
    rename_req.append(rename_info.data(), rename_info.size());

    auto rename_span = rename_req.readable_span();
    auto rename_resp = disp.dispatch(*session, hdr,
                                     reinterpret_cast<const uint8_t *>(rename_span.data()),
                                     rename_span.size());
    auto rename_resp_span = rename_resp.readable_span();
    auto rename_resp_hdr = Smb2Codec::decode_header(
        reinterpret_cast<const uint8_t *>(rename_resp_span.data()), rename_resp_span.size());
    TEST_ASSERT(rename_resp_hdr.has_value(), "Should decode denied rename set_info response");
    TEST_ASSERT(rename_resp_hdr->status == static_cast<uint32_t>(NtStatus::ACCESS_DENIED),
                "Denied rename should map to ACCESS_DENIED");
    TEST_ASSERT(handler.rename_called, "Denied rename should trigger handler on_rename hook");
    TEST_ASSERT(handler.last_rename_path == "new.txt", "Rename hook should receive target path");
    TEST_ASSERT(std::filesystem::exists(test_dir + "/old.txt"), "Denied rename should not mutate file");

    uint8_t delete_info[1] = { 1 };
    ByteBuffer delete_req = Smb2Codec::encode_header(hdr);
    Smb2Codec::write_le16(delete_req, 33);
    delete_req.append_u8(SMB2_0_INFO_FILE);
    delete_req.append_u8(static_cast<uint8_t>(FileInfoClass::FileDispositionInformation));
    Smb2Codec::write_le32(delete_req, sizeof(delete_info));
    Smb2Codec::write_le16(delete_req, SMB2_HEADER_SIZE + 32);
    Smb2Codec::write_le16(delete_req, 0);
    Smb2Codec::write_le32(delete_req, 0);
    Smb2Codec::write_le64(delete_req, fid.persistent);
    Smb2Codec::write_le64(delete_req, fid.volatile_id);
    delete_req.append(delete_info, sizeof(delete_info));

    auto delete_span = delete_req.readable_span();
    auto delete_resp = disp.dispatch(*session, hdr,
                                     reinterpret_cast<const uint8_t *>(delete_span.data()),
                                     delete_span.size());
    auto delete_resp_span = delete_resp.readable_span();
    auto delete_resp_hdr = Smb2Codec::decode_header(
        reinterpret_cast<const uint8_t *>(delete_resp_span.data()), delete_resp_span.size());
    TEST_ASSERT(delete_resp_hdr.has_value(), "Should decode denied delete set_info response");
    TEST_ASSERT(delete_resp_hdr->status == static_cast<uint32_t>(NtStatus::ACCESS_DENIED),
                "Denied delete should map to ACCESS_DENIED");
    TEST_ASSERT(handler.delete_called, "Denied delete should trigger handler on_delete hook");
    TEST_ASSERT(std::filesystem::exists(test_dir + "/old.txt"), "Denied delete should not remove file");

    share->remove_open_file(fid);
    fs_ptr->close(open_result.handle);
    session_mgr.remove_session(session->session_id());
    std::filesystem::remove_all(test_dir);
    return true;
}

bool test_dispatcher_echo()
{
    SmbServerConfig config;
    SmbShareManager share_mgr;
    SmbLockManager lock_mgr;
    SmbPipeManager pipe_mgr;
    SmbDfsResolver dfs;
    SmbChangeNotifier cn;

    SmbDispatcher disp(config, share_mgr, lock_mgr, pipe_mgr, dfs, cn);

    SmbSessionManager session_mgr;
    SmbSession *session = session_mgr.create_session(nullptr);
    session->set_state(SmbSession::State::authenticated);

    Smb2Header hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.protocol_id = SMB2_PROTOCOL_ID;
    hdr.command = static_cast<uint16_t>(Smb2Command::ECHO);
    hdr.credit_request = 1;

    ByteBuffer echo_buf = Smb2Codec::encode_header(hdr);
    Smb2Codec::write_le16(echo_buf, 4);
    Smb2Codec::write_le16(echo_buf, 0);

    auto span = echo_buf.readable_span();
    auto resp = disp.dispatch(*session, hdr,
                              reinterpret_cast<const uint8_t *>(span.data()),
                              span.size());

    TEST_ASSERT(resp.readable_bytes() >= SMB2_HEADER_SIZE, "Echo response should have header");

    auto resp_span = resp.readable_span();
    auto resp_hdr = Smb2Codec::decode_header(
        reinterpret_cast<const uint8_t *>(resp_span.data()), resp_span.size());
    TEST_ASSERT(resp_hdr.has_value(), "Should decode echo response");
    TEST_ASSERT(resp_hdr->command == static_cast<uint16_t>(Smb2Command::ECHO),
                "Response command should be ECHO");

    session_mgr.remove_session(session->session_id());
    return true;
}

bool test_transform_header_encode_decode()
{
    Smb2TransformHeader th;
    std::memset(&th, 0, sizeof(th));
    th.protocol_id = SMB2_TRANSFORM_PROTOCOL_ID;
    th.session_id = 0x123456789ABCDEF0ULL;
    for (int i = 0; i < 16; ++i)
        th.nonce[i] = static_cast<uint8_t>(i);
    th.original_message_size = 1024;

    auto encoded = Smb2Codec::encode_transform_header(th);
    TEST_ASSERT(encoded.readable_bytes() >= 52, "Transform header should be at least 52 bytes");

    auto span = encoded.readable_span();
    TEST_ASSERT(Smb2Codec::is_transform_header(
                    reinterpret_cast<const uint8_t *>(span.data()), span.size()),
                "Should detect transform header");

    auto decoded = Smb2Codec::decode_transform_header(
        reinterpret_cast<const uint8_t *>(span.data()), span.size());
    TEST_ASSERT(decoded.has_value(), "Should decode transform header");
    TEST_ASSERT(decoded->session_id == th.session_id, "Session ID should match");
    TEST_ASSERT(decoded->original_message_size == 1024, "Original message size should match");

    return true;
}

bool test_make_response_header()
{
    Smb2Header req;
    std::memset(&req, 0, sizeof(req));
    req.protocol_id = SMB2_PROTOCOL_ID;
    req.command = 0x0005;
    req.message_id = 42;
    req.session_id = 100;
    req.tree_id = 5;
    req.credit_charge = 1;

    auto resp = Smb2Codec::make_response_header(req, 0x0005, 2);
    TEST_ASSERT(resp.flags == SMB2_FLAGS_SERVER_TO_REDIR, "Response should have server-to-redir flag");
    TEST_ASSERT(resp.credit_request == 2, "Credit grant should match");
    TEST_ASSERT(resp.message_id == 42, "Message ID should match");
    TEST_ASSERT(resp.session_id == 100, "Session ID should match");
    TEST_ASSERT(resp.tree_id == 5, "Tree ID should match");

    return true;
}

bool test_session_setup_response_layout()
{
    Smb2Header hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.protocol_id = SMB2_PROTOCOL_ID;
    hdr.command = static_cast<uint16_t>(Smb2Command::SESSION_SETUP);
    hdr.session_id = 0x1122334455667788ULL;

    Smb2SessionSetupResponse resp;
    resp.session_flags = 0;
    resp.security_buffer = {0x60, 0x03, 0x06, 0x01};

    auto encoded = Smb2Codec::encode_session_setup_response(hdr, resp);
    auto span = encoded.readable_span();
    const auto *raw = reinterpret_cast<const uint8_t *>(span.data());

    TEST_ASSERT(Smb2Codec::read_le16(raw + SMB2_HEADER_SIZE) == 9,
                "Session setup structure size should be 9");
    TEST_ASSERT(Smb2Codec::read_le16(raw + SMB2_HEADER_SIZE + 2) == 0,
                "Session flags should be 0");
    TEST_ASSERT(Smb2Codec::read_le16(raw + SMB2_HEADER_SIZE + 4) == SMB2_HEADER_SIZE + 8,
                "Session setup security buffer offset should follow fixed body");
    TEST_ASSERT(Smb2Codec::read_le16(raw + SMB2_HEADER_SIZE + 6) == resp.security_buffer.size(),
                "Session setup security buffer length should match");

    size_t sec_offset = SMB2_HEADER_SIZE + 8;
    TEST_ASSERT(std::memcmp(raw + sec_offset, resp.security_buffer.data(), resp.security_buffer.size()) == 0,
                "Security buffer bytes must start immediately after fixed body with no padding");

    size_t expected_total = SMB2_HEADER_SIZE + 8 + resp.security_buffer.size();
    TEST_ASSERT(span.size() == expected_total,
                "Total response size should be header + fixed body (8) + security buffer, no padding");

    return true;
}

int main()
{
    

    std::cout << "=== SMB Protocol Unit Tests ===" << std::endl;

    std::cout << "\n--- Constants & Codec ---" << std::endl;
    RUN_TEST(test_smb2_constants);
    RUN_TEST(test_smb2_header_encode_decode);
    RUN_TEST(test_smb2_is_header_detection);
    RUN_TEST(test_le_read_write);
    RUN_TEST(test_utf8_utf16le_conversion);
    RUN_TEST(test_filetime_now);
    RUN_TEST(test_error_response);
    RUN_TEST(test_make_response_header);
    RUN_TEST(test_session_setup_response_layout);
    RUN_TEST(test_transform_header_encode_decode);

    std::cout << "\n--- NetBIOS ---" << std::endl;
    RUN_TEST(test_netbios_encode_decode);
    RUN_TEST(test_netbios_split_messages);

    std::cout << "\n--- SMB1 Negotiate ---" << std::endl;
    RUN_TEST(test_smb1_negotiate_detection);
    RUN_TEST(test_smb1_redirect_response);

    std::cout << "\n--- Config ---" << std::endl;
    RUN_TEST(test_server_config_defaults);
    RUN_TEST(test_share_config);

    std::cout << "\n--- Share Manager ---" << std::endl;
    RUN_TEST(test_share_manager);

    std::cout << "\n--- Session Manager ---" << std::endl;
    RUN_TEST(test_session_manager);
    RUN_TEST(test_session_tree_connections);
    RUN_TEST(test_session_crypto_keys);

    std::cout << "\n--- Lock Manager ---" << std::endl;
    RUN_TEST(test_lock_manager_basic);
    RUN_TEST(test_lock_manager_oplock);
    RUN_TEST(test_lock_manager_lease);

    std::cout << "\n--- Pipe Manager ---" << std::endl;
    RUN_TEST(test_pipe_manager_basic);
    RUN_TEST(test_pipe_manager_custom_handlers);

    std::cout << "\n--- DFS Resolver ---" << std::endl;
    RUN_TEST(test_dfs_resolver);

    std::cout << "\n--- Change Notifier ---" << std::endl;
    RUN_TEST(test_change_notifier);

    std::cout << "\n--- Authentication ---" << std::endl;
    RUN_TEST(test_ntlm_auth);
    RUN_TEST(test_ntlmv2_password_proof_auth);
    RUN_TEST(test_ntlmv2_nt_hash_proof_auth);
    RUN_TEST(test_spnego_auth);
    RUN_TEST(test_spnego_auth_accepts_oid_only_init);
    RUN_TEST(test_spnego_auth_extracts_mechtoken_from_der_init);

    std::cout << "\n--- Crypto ---" << std::endl;
    RUN_TEST(test_crypto_openssl_sha512);
    RUN_TEST(test_crypto_openssl_hmac);
    RUN_TEST(test_crypto_sign_verify);
    RUN_TEST(test_crypto_encrypt_decrypt);

    std::cout << "\n--- Key Derivation ---" << std::endl;
    RUN_TEST(test_key_derivation);

    std::cout << "\n--- Server ---" << std::endl;
    RUN_TEST(test_server_creation);
    RUN_TEST(test_server_share_init);

    std::cout << "\n--- File System ---" << std::endl;
    RUN_TEST(test_local_file_system);
    RUN_TEST(test_local_file_system_directory);
    RUN_TEST(test_local_file_system_set_info_rename_delete);
    RUN_TEST(test_local_file_system_absolute_rename);
    RUN_TEST(test_local_file_system_directory_cursor);

    std::cout << "\n--- Dispatcher ---" << std::endl;
    RUN_TEST(test_dispatcher_negotiate);
    RUN_TEST(test_dispatcher_negotiate_conservative_capabilities);
    RUN_TEST(test_dispatcher_negotiate_signing_required_mode);
    RUN_TEST(test_negotiate_response_layout_for_smb311);
    RUN_TEST(test_dispatcher_query_filesystem_info);
    RUN_TEST(test_dispatcher_ioctl_validate_negotiate_info);
    RUN_TEST(test_dispatcher_ioctl_query_network_interface_info);
    RUN_TEST(test_dispatcher_ioctl_permission_denied);
    RUN_TEST(test_dispatcher_change_notify_completes_empty);
    RUN_TEST(test_dispatcher_set_info_permission_hooks);
    RUN_TEST(test_dispatcher_echo);

    std::cout << "\n=== Results: " << g_tests_passed << "/" << g_tests_run
              << " passed, " << g_tests_failed << " failed ===" << std::endl;

    return g_tests_failed > 0 ? 1 : 0;
}
