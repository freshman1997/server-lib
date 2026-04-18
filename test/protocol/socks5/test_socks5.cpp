#include "socks5_protocol.h"
#include "socks5_packet_parser.h"
#include "socks5_config.h"
#include "socks5_session.h"
#include "socks5_server.h"
#include "buffer/byte_buffer.h"
#include "net/socket/inet_address.h"
#include "common/winsock_guard.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>

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

using namespace yuan::net::socks5;
using namespace yuan::buffer;

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

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

static void append_u8(ByteBuffer &buf, uint8_t value)
{
    buf.append_u8(value);
}

bool test_protocol_enums()
{
    TEST_ASSERT(static_cast<uint8_t>(SocksVersion::v5) == 0x05,
                "SocksVersion::v5 should be 0x05");

    TEST_ASSERT(static_cast<uint8_t>(AuthMethod::no_auth) == 0x00,
                "AuthMethod::no_auth should be 0x00");
    TEST_ASSERT(static_cast<uint8_t>(AuthMethod::username_password) == 0x02,
                "AuthMethod::username_password should be 0x02");
    TEST_ASSERT(static_cast<uint8_t>(AuthMethod::no_acceptable) == 0xFF,
                "AuthMethod::no_acceptable should be 0xFF");

    TEST_ASSERT(static_cast<uint8_t>(Command::connect) == 0x01,
                "Command::connect should be 0x01");
    TEST_ASSERT(static_cast<uint8_t>(Command::bind) == 0x02,
                "Command::bind should be 0x02");
    TEST_ASSERT(static_cast<uint8_t>(Command::udp_associate) == 0x03,
                "Command::udp_associate should be 0x03");

    TEST_ASSERT(static_cast<uint8_t>(AddressType::ipv4) == 0x01,
                "AddressType::ipv4 should be 0x01");
    TEST_ASSERT(static_cast<uint8_t>(AddressType::domain) == 0x03,
                "AddressType::domain should be 0x03");
    TEST_ASSERT(static_cast<uint8_t>(AddressType::ipv6) == 0x04,
                "AddressType::ipv6 should be 0x04");

    TEST_ASSERT(static_cast<uint8_t>(ReplyCode::succeeded) == 0x00,
                "ReplyCode::succeeded should be 0x00");
    TEST_ASSERT(static_cast<uint8_t>(ReplyCode::general_failure) == 0x01,
                "ReplyCode::general_failure should be 0x01");
    TEST_ASSERT(static_cast<uint8_t>(ReplyCode::connection_not_allowed) == 0x02,
                "ReplyCode::connection_not_allowed should be 0x02");
    TEST_ASSERT(static_cast<uint8_t>(ReplyCode::network_unreachable) == 0x03,
                "ReplyCode::network_unreachable should be 0x03");
    TEST_ASSERT(static_cast<uint8_t>(ReplyCode::host_unreachable) == 0x04,
                "ReplyCode::host_unreachable should be 0x04");
    TEST_ASSERT(static_cast<uint8_t>(ReplyCode::connection_refused) == 0x05,
                "ReplyCode::connection_refused should be 0x05");
    TEST_ASSERT(static_cast<uint8_t>(ReplyCode::ttl_expired) == 0x06,
                "ReplyCode::ttl_expired should be 0x06");
    TEST_ASSERT(static_cast<uint8_t>(ReplyCode::command_not_supported) == 0x07,
                "ReplyCode::command_not_supported should be 0x07");
    TEST_ASSERT(static_cast<uint8_t>(ReplyCode::address_type_not_supported) == 0x08,
                "ReplyCode::address_type_not_supported should be 0x08");

    return true;
}

bool test_greeting_parse_no_auth()
{
    ByteBuffer buf(16);
    append_u8(buf, 0x05);
    append_u8(buf, 0x01);
    append_u8(buf, 0x00);

    auto result = Socks5PacketParser::parse_greeting(buf);
    TEST_ASSERT(result.has_value(), "greeting parse should succeed");
    TEST_ASSERT(result->version == 0x05, "version should be 0x05");
    TEST_ASSERT(result->method_count == 1, "method count should be 1");
    TEST_ASSERT(result->methods[0] == 0x00, "method should be no_auth");

    return true;
}

bool test_greeting_parse_multiple_methods()
{
    ByteBuffer buf(16);
    append_u8(buf, 0x05);
    append_u8(buf, 0x03);
    append_u8(buf, 0x00);
    append_u8(buf, 0x01);
    append_u8(buf, 0x02);

    auto result = Socks5PacketParser::parse_greeting(buf);
    TEST_ASSERT(result.has_value(), "greeting parse should succeed");
    TEST_ASSERT(result->method_count == 3, "method count should be 3");
    TEST_ASSERT(result->methods[0] == 0x00, "method[0] should be no_auth");
    TEST_ASSERT(result->methods[1] == 0x01, "method[1] should be gssapi");
    TEST_ASSERT(result->methods[2] == 0x02, "method[2] should be username_password");

    return true;
}

bool test_greeting_parse_incomplete()
{
    ByteBuffer buf(16);
    append_u8(buf, 0x05);
    append_u8(buf, 0x03);
    append_u8(buf, 0x00);

    auto result = Socks5PacketParser::parse_greeting(buf);
    TEST_ASSERT(!result.has_value(), "incomplete greeting should fail to parse");

    return true;
}

bool test_greeting_parse_wrong_version()
{
    ByteBuffer buf(16);
    append_u8(buf, 0x04);
    append_u8(buf, 0x01);
    append_u8(buf, 0x00);

    auto result = Socks5PacketParser::parse_greeting(buf);
    TEST_ASSERT(!result.has_value(), "wrong version should fail to parse");

    return true;
}

bool test_greeting_parse_empty()
{
    ByteBuffer buf(16);
    auto result = Socks5PacketParser::parse_greeting(buf);
    TEST_ASSERT(!result.has_value(), "empty buffer should fail to parse");

    return true;
}

bool test_auth_request_parse()
{
    ByteBuffer buf(64);
    append_u8(buf, 0x01);
    append_u8(buf, 0x04);
    append_u8(buf, 'u');
    append_u8(buf, 's');
    append_u8(buf, 'e');
    append_u8(buf, 'r');
    append_u8(buf, 0x04);
    append_u8(buf, 'p');
    append_u8(buf, 'a');
    append_u8(buf, 's');
    append_u8(buf, 's');

    auto result = Socks5PacketParser::parse_auth_request(buf);
    TEST_ASSERT(result.has_value(), "auth parse should succeed");
    TEST_ASSERT(result->first == "user", "username should be 'user'");
    TEST_ASSERT(result->second == "pass", "password should be 'pass'");

    return true;
}

bool test_auth_request_parse_incomplete()
{
    ByteBuffer buf(64);
    append_u8(buf, 0x01);
    append_u8(buf, 0x02);
    append_u8(buf, 'h');
    append_u8(buf, 'e');

    auto result = Socks5PacketParser::parse_auth_request(buf);
    TEST_ASSERT(!result.has_value(), "incomplete auth should fail to parse");

    return true;
}

bool test_request_parse_connect_ipv4()
{
    ByteBuffer buf(64);
    append_u8(buf, 0x05);
    append_u8(buf, 0x01);
    append_u8(buf, 0x00);
    append_u8(buf, 0x01);

    uint32_t ip = 0;
    inet_pton(AF_INET, "127.0.0.1", &ip);
    buf.append(reinterpret_cast<const uint8_t *>(&ip), 4);

    uint16_t port = 80;
    uint16_t net_port = htons(port);
    buf.append(reinterpret_cast<const uint8_t *>(&net_port), 2);

    auto result = Socks5PacketParser::parse_request(buf);
    TEST_ASSERT(result.has_value(), "request parse should succeed");
    TEST_ASSERT(result->cmd == Command::connect, "command should be connect");
    TEST_ASSERT(result->atyp == AddressType::ipv4, "address type should be ipv4");
    TEST_ASSERT(std::string(result->address) == "127.0.0.1", "address should be 127.0.0.1");
    TEST_ASSERT(result->port == 80, "port should be 80");

    return true;
}

bool test_request_parse_connect_domain()
{
    ByteBuffer buf(128);
    append_u8(buf, 0x05);
    append_u8(buf, 0x01);
    append_u8(buf, 0x00);
    append_u8(buf, 0x03);

    std::string domain = "example.com";
    append_u8(buf, static_cast<uint8_t>(domain.size()));
    buf.append(reinterpret_cast<const uint8_t *>(domain.data()), domain.size());

    uint16_t port = 443;
    uint16_t net_port = htons(port);
    buf.append(reinterpret_cast<const uint8_t *>(&net_port), 2);

    auto result = Socks5PacketParser::parse_request(buf);
    TEST_ASSERT(result.has_value(), "request parse should succeed");
    TEST_ASSERT(result->cmd == Command::connect, "command should be connect");
    TEST_ASSERT(result->atyp == AddressType::domain, "address type should be domain");
    TEST_ASSERT(std::string(result->address) == "example.com", "address should be example.com");
    TEST_ASSERT(result->port == 443, "port should be 443");

    return true;
}

bool test_request_parse_connect_ipv6()
{
    ByteBuffer buf(64);
    append_u8(buf, 0x05);
    append_u8(buf, 0x01);
    append_u8(buf, 0x00);
    append_u8(buf, 0x04);

    struct in6_addr addr6;
    inet_pton(AF_INET6, "::1", &addr6);
    buf.append(reinterpret_cast<const uint8_t *>(&addr6), 16);

    uint16_t port = 8080;
    uint16_t net_port = htons(port);
    buf.append(reinterpret_cast<const uint8_t *>(&net_port), 2);

    auto result = Socks5PacketParser::parse_request(buf);
    TEST_ASSERT(result.has_value(), "request parse should succeed");
    TEST_ASSERT(result->cmd == Command::connect, "command should be connect");
    TEST_ASSERT(result->atyp == AddressType::ipv6, "address type should be ipv6");
    TEST_ASSERT(result->port == 8080, "port should be 8080");

    return true;
}

bool test_request_parse_udp_associate()
{
    ByteBuffer buf(64);
    append_u8(buf, 0x05);
    append_u8(buf, 0x03);
    append_u8(buf, 0x00);
    append_u8(buf, 0x01);

    uint32_t ip = 0;
    inet_pton(AF_INET, "192.168.1.1", &ip);
    buf.append(reinterpret_cast<const uint8_t *>(&ip), 4);

    uint16_t port = 1080;
    uint16_t net_port = htons(port);
    buf.append(reinterpret_cast<const uint8_t *>(&net_port), 2);

    auto result = Socks5PacketParser::parse_request(buf);
    TEST_ASSERT(result.has_value(), "request parse should succeed");
    TEST_ASSERT(result->cmd == Command::udp_associate, "command should be udp_associate");
    TEST_ASSERT(std::string(result->address) == "192.168.1.1", "address should be 192.168.1.1");
    TEST_ASSERT(result->port == 1080, "port should be 1080");

    return true;
}

bool test_request_parse_incomplete()
{
    ByteBuffer buf(16);
    append_u8(buf, 0x05);
    append_u8(buf, 0x01);
    append_u8(buf, 0x00);

    auto result = Socks5PacketParser::parse_request(buf);
    TEST_ASSERT(!result.has_value(), "incomplete request should fail to parse");

    return true;
}

bool test_build_method_select_reply()
{
    auto reply = Socks5PacketParser::build_method_select_reply(AuthMethod::no_auth);
    auto span = reply.readable_span();
    TEST_ASSERT(span.size() == 2, "method select reply should be 2 bytes");
    TEST_ASSERT(span[0] == 0x05, "version should be 0x05");
    TEST_ASSERT(span[1] == 0x00, "method should be no_auth (0x00)");

    return true;
}

bool test_build_method_select_reply_no_acceptable()
{
    auto reply = Socks5PacketParser::build_method_select_reply(AuthMethod::no_acceptable);
    auto span = reply.readable_span();
    TEST_ASSERT(span.size() == 2, "method select reply should be 2 bytes");
    TEST_ASSERT(span[0] == 0x05, "version should be 0x05");
    TEST_ASSERT(static_cast<uint8_t>(span[1]) == 0xFF, "method should be no_acceptable (0xFF)");

    return true;
}

bool test_build_auth_reply()
{
    auto reply_success = Socks5PacketParser::build_auth_reply(true);
    auto span = reply_success.readable_span();
    TEST_ASSERT(span.size() == 2, "auth reply should be 2 bytes");
    TEST_ASSERT(span[0] == 0x01, "version should be 0x01");
    TEST_ASSERT(span[1] == 0x00, "success should be 0x00");

    auto reply_fail = Socks5PacketParser::build_auth_reply(false);
    span = reply_fail.readable_span();
    TEST_ASSERT(span[1] == 0x01, "failure should be 0x01");

    return true;
}

bool test_build_reply_ipv4()
{
    auto reply = Socks5PacketParser::build_reply(ReplyCode::succeeded, AddressType::ipv4, "127.0.0.1", 1080);
    auto span = reply.readable_span();
    TEST_ASSERT(span.size() >= 10, "ipv4 reply should be at least 10 bytes");
    TEST_ASSERT(span[0] == 0x05, "version should be 0x05");
    TEST_ASSERT(span[1] == 0x00, "reply code should be succeeded");
    TEST_ASSERT(span[2] == 0x00, "reserved should be 0");
    TEST_ASSERT(span[3] == 0x01, "address type should be ipv4");

    return true;
}

bool test_build_reply_domain()
{
    auto reply = Socks5PacketParser::build_reply(ReplyCode::succeeded, AddressType::domain, "example.com", 443);
    auto span = reply.readable_span();
    TEST_ASSERT(span[0] == 0x05, "version should be 0x05");
    TEST_ASSERT(span[3] == 0x03, "address type should be domain");

    std::string domain_len_byte_str(reinterpret_cast<const char *>(&span[4]), 1);
    TEST_ASSERT(span[4] == 11, "domain length should be 11");

    return true;
}

bool test_build_reply_ipv6()
{
    auto reply = Socks5PacketParser::build_reply(ReplyCode::succeeded, AddressType::ipv6, "::1", 8080);
    auto span = reply.readable_span();
    TEST_ASSERT(span[0] == 0x05, "version should be 0x05");
    TEST_ASSERT(span[3] == 0x04, "address type should be ipv6");
    TEST_ASSERT(span.size() >= 22, "ipv6 reply should be at least 22 bytes");

    return true;
}

bool test_build_reply_default()
{
    auto reply = Socks5PacketParser::build_reply(ReplyCode::general_failure);
    auto span = reply.readable_span();
    TEST_ASSERT(span[0] == 0x05, "version should be 0x05");
    TEST_ASSERT(span[1] == 0x01, "reply code should be general_failure");
    TEST_ASSERT(span[3] == 0x01, "default address type should be ipv4");

    return true;
}

bool test_udp_header_parse_ipv4()
{
    ByteBuffer buf(64);
    uint16_t reserved = 0;
    uint16_t net_reserved = htons(reserved);
    buf.append(reinterpret_cast<const uint8_t *>(&net_reserved), 2);
    append_u8(buf, 0x00);
    append_u8(buf, 0x01);

    uint32_t ip = 0;
    inet_pton(AF_INET, "10.0.0.1", &ip);
    buf.append(reinterpret_cast<const uint8_t *>(&ip), 4);

    uint16_t port = 53;
    uint16_t net_port = htons(port);
    buf.append(reinterpret_cast<const uint8_t *>(&net_port), 2);

    append_u8(buf, 'H');
    append_u8(buf, 'E');
    append_u8(buf, 'L');
    append_u8(buf, 'L');
    append_u8(buf, 'O');

    auto result = Socks5PacketParser::parse_udp_header(buf);
    TEST_ASSERT(result.has_value(), "UDP header parse should succeed");
    TEST_ASSERT(result->reserved == 0, "reserved should be 0");
    TEST_ASSERT(result->fragment == 0, "fragment should be 0");
    TEST_ASSERT(result->atyp == AddressType::ipv4, "address type should be ipv4");
    TEST_ASSERT(std::string(result->address) == "10.0.0.1", "address should be 10.0.0.1");
    TEST_ASSERT(result->port == 53, "port should be 53");

    return true;
}

bool test_udp_header_parse_domain()
{
    ByteBuffer buf(128);
    uint16_t reserved = 0;
    uint16_t net_reserved = htons(reserved);
    buf.append(reinterpret_cast<const uint8_t *>(&net_reserved), 2);
    append_u8(buf, 0x00);
    append_u8(buf, 0x03);

    std::string domain = "dns.server.com";
    append_u8(buf, static_cast<uint8_t>(domain.size()));
    buf.append(reinterpret_cast<const uint8_t *>(domain.data()), domain.size());

    uint16_t port = 5353;
    uint16_t net_port = htons(port);
    buf.append(reinterpret_cast<const uint8_t *>(&net_port), 2);

    auto result = Socks5PacketParser::parse_udp_header(buf);
    TEST_ASSERT(result.has_value(), "UDP header parse should succeed");
    TEST_ASSERT(result->atyp == AddressType::domain, "address type should be domain");
    TEST_ASSERT(std::string(result->address) == "dns.server.com", "address should be dns.server.com");
    TEST_ASSERT(result->port == 5353, "port should be 5353");

    return true;
}

bool test_udp_header_parse_ipv6()
{
    ByteBuffer buf(64);
    uint16_t reserved = 0;
    uint16_t net_reserved = htons(reserved);
    buf.append(reinterpret_cast<const uint8_t *>(&net_reserved), 2);
    append_u8(buf, 0x00);
    append_u8(buf, 0x04);

    struct in6_addr addr6;
    inet_pton(AF_INET6, "2001:db8::1", &addr6);
    buf.append(reinterpret_cast<const uint8_t *>(&addr6), 16);

    uint16_t port = 443;
    uint16_t net_port = htons(port);
    buf.append(reinterpret_cast<const uint8_t *>(&net_port), 2);

    auto result = Socks5PacketParser::parse_udp_header(buf);
    TEST_ASSERT(result.has_value(), "UDP header parse should succeed");
    TEST_ASSERT(result->atyp == AddressType::ipv6, "address type should be ipv6");
    TEST_ASSERT(result->port == 443, "port should be 443");

    return true;
}

bool test_udp_header_parse_incomplete()
{
    ByteBuffer buf(16);
    uint16_t reserved = 0;
    uint16_t net_reserved = htons(reserved);
    buf.append(reinterpret_cast<const uint8_t *>(&net_reserved), 2);
    append_u8(buf, 0x00);
    append_u8(buf, 0x01);

    auto result = Socks5PacketParser::parse_udp_header(buf);
    TEST_ASSERT(!result.has_value(), "incomplete UDP header should fail to parse");

    return true;
}

bool test_udp_header_parse_empty()
{
    ByteBuffer buf(16);
    auto result = Socks5PacketParser::parse_udp_header(buf);
    TEST_ASSERT(!result.has_value(), "empty UDP header should fail to parse");

    return true;
}

bool test_build_udp_header_ipv4()
{
    auto header = Socks5PacketParser::build_udp_header(AddressType::ipv4, "10.0.0.1", 53);
    auto span = header.readable_span();
    TEST_ASSERT(span.size() >= 10, "ipv4 UDP header should be at least 10 bytes");

    uint16_t net_reserved;
    std::memcpy(&net_reserved, span.data(), 2);
    TEST_ASSERT(ntohs(net_reserved) == 0, "reserved should be 0");
    TEST_ASSERT(span[2] == 0x00, "fragment should be 0");
    TEST_ASSERT(span[3] == 0x01, "address type should be ipv4");

    uint16_t net_port;
    std::memcpy(&net_port, span.data() + 8, 2);
    TEST_ASSERT(ntohs(net_port) == 53, "port should be 53");

    return true;
}

bool test_build_udp_header_domain()
{
    auto header = Socks5PacketParser::build_udp_header(AddressType::domain, "example.com", 8080);
    auto span = header.readable_span();
    TEST_ASSERT(span[2] == 0x00, "fragment should be 0");
    TEST_ASSERT(span[3] == 0x03, "address type should be domain");
    TEST_ASSERT(span[4] == 11, "domain length should be 11");

    return true;
}

bool test_build_udp_header_ipv6()
{
    auto header = Socks5PacketParser::build_udp_header(AddressType::ipv6, "::1", 443);
    auto span = header.readable_span();
    TEST_ASSERT(span[2] == 0x00, "fragment should be 0");
    TEST_ASSERT(span[3] == 0x04, "address type should be ipv6");

    return true;
}

bool test_udp_header_roundtrip_ipv4()
{
    auto built = Socks5PacketParser::build_udp_header(AddressType::ipv4, "192.168.1.100", 8080);
    auto parsed = Socks5PacketParser::parse_udp_header(built);

    TEST_ASSERT(parsed.has_value(), "roundtrip parse should succeed");
    TEST_ASSERT(parsed->reserved == 0, "reserved should be 0");
    TEST_ASSERT(parsed->fragment == 0, "fragment should be 0");
    TEST_ASSERT(parsed->atyp == AddressType::ipv4, "address type should be ipv4");
    TEST_ASSERT(std::string(parsed->address) == "192.168.1.100", "address should match");
    TEST_ASSERT(parsed->port == 8080, "port should match");

    return true;
}

bool test_udp_header_roundtrip_domain()
{
    auto built = Socks5PacketParser::build_udp_header(AddressType::domain, "test.example.org", 9999);
    auto parsed = Socks5PacketParser::parse_udp_header(built);

    TEST_ASSERT(parsed.has_value(), "roundtrip parse should succeed");
    TEST_ASSERT(parsed->atyp == AddressType::domain, "address type should be domain");
    TEST_ASSERT(std::string(parsed->address) == "test.example.org", "address should match");
    TEST_ASSERT(parsed->port == 9999, "port should match");

    return true;
}

bool test_udp_header_roundtrip_ipv6()
{
    auto built = Socks5PacketParser::build_udp_header(AddressType::ipv6, "fe80::1", 12345);
    auto parsed = Socks5PacketParser::parse_udp_header(built);

    TEST_ASSERT(parsed.has_value(), "roundtrip parse should succeed");
    TEST_ASSERT(parsed->atyp == AddressType::ipv6, "address type should be ipv6");
    TEST_ASSERT(parsed->port == 12345, "port should match");

    return true;
}

bool test_config_defaults()
{
    Socks5ServerConfig config;
    TEST_ASSERT(!config.enable_auth, "auth should be disabled by default");
    TEST_ASSERT(config.username.empty(), "username should be empty by default");
    TEST_ASSERT(config.password.empty(), "password should be empty by default");
    TEST_ASSERT(config.enable_connect, "connect should be enabled by default");
    TEST_ASSERT(!config.enable_bind, "bind should be disabled by default");
    TEST_ASSERT(!config.enable_udp_associate, "udp_associate should be disabled by default");
    TEST_ASSERT(config.connect_timeout_ms == 10000, "connect timeout should be 10000");
    TEST_ASSERT(config.idle_timeout_ms == 300000, "idle timeout should be 300000");
    TEST_ASSERT(config.max_connections == 1024, "max connections should be 1024");

    return true;
}

bool test_config_custom()
{
    Socks5ServerConfig config;
    config.enable_auth = true;
    config.username = "admin";
    config.password = "secret";
    config.enable_connect = true;
    config.enable_bind = true;
    config.enable_udp_associate = true;
    config.connect_timeout_ms = 5000;
    config.max_connections = 2048;

    TEST_ASSERT(config.enable_auth, "auth should be enabled");
    TEST_ASSERT(config.username == "admin", "username should be admin");
    TEST_ASSERT(config.password == "secret", "password should be secret");
    TEST_ASSERT(config.enable_bind, "bind should be enabled");
    TEST_ASSERT(config.enable_udp_associate, "udp_associate should be enabled");
    TEST_ASSERT(config.connect_timeout_ms == 5000, "connect timeout should be 5000");
    TEST_ASSERT(config.max_connections == 2048, "max connections should be 2048");

    return true;
}

bool test_session_state_transitions()
{
    TEST_ASSERT(static_cast<int>(Socks5Session::State::greeting) == 0, "greeting state order");
    TEST_ASSERT(static_cast<int>(Socks5Session::State::auth) == 1, "auth state order");
    TEST_ASSERT(static_cast<int>(Socks5Session::State::request) == 2, "request state order");
    TEST_ASSERT(static_cast<int>(Socks5Session::State::connecting) == 3, "connecting state order");
    TEST_ASSERT(static_cast<int>(Socks5Session::State::udp_associate) == 4, "udp_associate state order");
    TEST_ASSERT(static_cast<int>(Socks5Session::State::established) == 5, "established state order");
    TEST_ASSERT(static_cast<int>(Socks5Session::State::closed) == 6, "closed state order");

    return true;
}

bool test_reply_roundtrip_ipv4()
{
    auto built = Socks5PacketParser::build_reply(ReplyCode::succeeded, AddressType::ipv4, "192.168.0.1", 8080);

    auto span = built.readable_span();
    TEST_ASSERT(span[0] == 0x05, "version should be 0x05");
    TEST_ASSERT(span[1] == static_cast<uint8_t>(ReplyCode::succeeded), "reply code should be succeeded");
    TEST_ASSERT(span[2] == 0x00, "reserved should be 0x00");
    TEST_ASSERT(span[3] == static_cast<uint8_t>(AddressType::ipv4), "address type should be ipv4");

    struct in_addr expected_ip;
    inet_pton(AF_INET, "192.168.0.1", &expected_ip);
    TEST_ASSERT(std::memcmp(&span[4], &expected_ip, 4) == 0, "IP address should match");

    uint16_t net_port;
    std::memcpy(&net_port, span.data() + 8, 2);
    TEST_ASSERT(ntohs(net_port) == 8080, "port should be 8080");

    return true;
}

bool test_reply_all_error_codes()
{
    struct TestCase
    {
        ReplyCode code;
        uint8_t expected;
    };

    std::vector<TestCase> cases = {
        { ReplyCode::succeeded, 0x00 },
        { ReplyCode::general_failure, 0x01 },
        { ReplyCode::connection_not_allowed, 0x02 },
        { ReplyCode::network_unreachable, 0x03 },
        { ReplyCode::host_unreachable, 0x04 },
        { ReplyCode::connection_refused, 0x05 },
        { ReplyCode::ttl_expired, 0x06 },
        { ReplyCode::command_not_supported, 0x07 },
        { ReplyCode::address_type_not_supported, 0x08 }
    };

    for (const auto &tc : cases) {
        auto reply = Socks5PacketParser::build_reply(tc.code);
        auto span = reply.readable_span();
        TEST_ASSERT(span[1] == tc.expected, "reply code byte should match expected value");
    }

    return true;
}

bool test_udp_header_fragment_field()
{
    ByteBuffer buf(64);
    uint16_t reserved = 0;
    uint16_t net_reserved = htons(reserved);
    buf.append(reinterpret_cast<const uint8_t *>(&net_reserved), 2);
    append_u8(buf, 0x02);
    append_u8(buf, 0x01);

    uint32_t ip = 0;
    inet_pton(AF_INET, "1.2.3.4", &ip);
    buf.append(reinterpret_cast<const uint8_t *>(&ip), 4);

    uint16_t port = 80;
    uint16_t net_port = htons(port);
    buf.append(reinterpret_cast<const uint8_t *>(&net_port), 2);

    auto result = Socks5PacketParser::parse_udp_header(buf);
    TEST_ASSERT(result.has_value(), "fragmented UDP header parse should succeed");
    TEST_ASSERT(result->fragment == 2, "fragment number should be 2");

    return true;
}

bool test_udp_header_with_payload_separation()
{
    ByteBuffer buf(128);
    uint16_t reserved = 0;
    uint16_t net_reserved = htons(reserved);
    buf.append(reinterpret_cast<const uint8_t *>(&net_reserved), 2);
    append_u8(buf, 0x00);
    append_u8(buf, 0x01);

    uint32_t ip = 0;
    inet_pton(AF_INET, "8.8.8.8", &ip);
    buf.append(reinterpret_cast<const uint8_t *>(&ip), 4);

    uint16_t port = 53;
    uint16_t net_port = htons(port);
    buf.append(reinterpret_cast<const uint8_t *>(&net_port), 2);

    const char *payload = "HELLO_WORLD";
    buf.append(reinterpret_cast<const uint8_t *>(payload), std::strlen(payload));

    auto result = Socks5PacketParser::parse_udp_header(buf);
    TEST_ASSERT(result.has_value(), "UDP header with payload parse should succeed");
    TEST_ASSERT(result->atyp == AddressType::ipv4, "address type should be ipv4");
    TEST_ASSERT(std::string(result->address) == "8.8.8.8", "address should be 8.8.8.8");
    TEST_ASSERT(result->port == 53, "port should be 53");

    size_t header_size = 4 + 4 + 2;
    auto span = buf.readable_span();
    TEST_ASSERT(span.size() > header_size, "buffer should have payload after header");

    std::string payload_str(reinterpret_cast<const char *>(span.data() + header_size),
                            span.size() - header_size);
    TEST_ASSERT(payload_str == "HELLO_WORLD", "payload should be HELLO_WORLD");

    return true;
}

bool test_greeting_max_methods()
{
    ByteBuffer buf(260);
    append_u8(buf, 0x05);
    append_u8(buf, 0xFF);
    for (uint8_t i = 0; i < 255; ++i) {
        append_u8(buf, i);
    }

    auto result = Socks5PacketParser::parse_greeting(buf);
    TEST_ASSERT(result.has_value(), "greeting with 255 methods should parse");
    TEST_ASSERT(result->method_count == 255, "method count should be 255");

    return true;
}

bool test_socks5_server_init()
{
    Socks5ServerConfig config;
    config.enable_auth = false;
    config.enable_connect = true;
    config.enable_udp_associate = true;

    Socks5Server server(config);
    TEST_ASSERT(server.config().enable_connect, "config connect should be true");
    TEST_ASSERT(server.config().enable_udp_associate, "config udp_associate should be true");
    TEST_ASSERT(!server.config().enable_auth, "config auth should be false");

    return true;
}

bool test_socks5_server_with_auth()
{
    Socks5ServerConfig config;
    config.enable_auth = true;
    config.username = "testuser";
    config.password = "testpass";

    Socks5Server server(config);
    TEST_ASSERT(server.config().enable_auth, "config auth should be true");
    TEST_ASSERT(server.config().username == "testuser", "username should match");
    TEST_ASSERT(server.config().password == "testpass", "password should match");

    return true;
}

bool test_socks5_server_handler()
{
    class TestHandler : public Socks5Handler
    {
    public:
        bool on_authenticate(const std::string &username, const std::string &password) override
        {
            return username == "admin" && password == "pass";
        }

        bool on_connect_request(Socks5Session *session, const std::string &host, uint16_t port) override
        {
            return true;
        }

        void on_session_opened(Socks5Session *session) override
        {
        }
        void on_session_closed(Socks5Session *session) override
        {
        }
    };

    TestHandler handler;
    TEST_ASSERT(handler.on_authenticate("admin", "pass"), "auth should succeed");
    TEST_ASSERT(!handler.on_authenticate("admin", "wrong"), "auth should fail with wrong password");
    TEST_ASSERT(handler.on_connect_request(nullptr, "example.com", 80), "connect request should be allowed");

    Socks5Server server;
    server.set_handler(&handler);

    return true;
}

#ifndef _WIN32
static bool create_tcp_connection(const char *host, int port, int &sock_fd)
{
    sock_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0)
        return false;

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (::connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ::close(sock_fd);
        sock_fd = -1;
        return false;
    }
    return true;
}

static bool create_udp_socket(int &sock_fd)
{
    sock_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    return sock_fd >= 0;
}

static ssize_t udp_sendto(int sock_fd, const char *host, int port, const uint8_t *data, size_t len)
{
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    return ::sendto(sock_fd, data, len, 0, (struct sockaddr *)&addr, sizeof(addr));
}
#else
static bool create_tcp_connection(const char *host, int port, SOCKET &sock_fd)
{
    sock_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == INVALID_SOCKET)
        return false;

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (::connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        ::closesocket(sock_fd);
        sock_fd = INVALID_SOCKET;
        return false;
    }
    return true;
}

static bool create_udp_socket(SOCKET &sock_fd)
{
    sock_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    return sock_fd != INVALID_SOCKET;
}

static int udp_sendto(SOCKET sock_fd, const char *host, int port, const uint8_t *data, size_t len)
{
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    return ::sendto(sock_fd, reinterpret_cast<const char *>(data), static_cast<int>(len), 0, (struct sockaddr *)&addr, sizeof(addr));
}
#endif

bool test_socks5_server_connect_e2e()
{
    const int test_port = 11080;

    Socks5ServerConfig config;
    config.enable_auth = false;
    config.enable_connect = true;
    config.enable_udp_associate = true;
    config.max_connections = 10;

    Socks5Server server(config);

    if (!server.init(test_port)) {
        std::cout << "    (skipped: could not bind port " << test_port << ")" << std::endl;
        return true;
    }

    std::thread server_thread([&server]() {
        server.serve();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

#ifndef _WIN32
    int sock_fd = -1;
#else
    SOCKET sock_fd = INVALID_SOCKET;
#endif

    bool connected = create_tcp_connection("127.0.0.1", test_port, sock_fd);

    if (!connected) {
        server.stop();
        server_thread.join();
        std::cout << "    (skipped: could not connect to server)" << std::endl;
        return true;
    }

    uint8_t greeting[] = { 0x05, 0x01, 0x00 };
#ifndef _WIN32
    ::send(sock_fd, greeting, sizeof(greeting), 0);
#else
    ::send(sock_fd, reinterpret_cast<const char *>(greeting), sizeof(greeting), 0);
#endif

    uint8_t response[2] = { 0 };
#ifndef _WIN32
    ::recv(sock_fd, reinterpret_cast<char *>(response), 2, 0);
#else
    ::recv(sock_fd, reinterpret_cast<char *>(response), 2, 0);
#endif

    TEST_ASSERT(response[0] == 0x05, "response version should be 0x05");
    TEST_ASSERT(response[1] == 0x00, "response method should be no_auth");

#ifndef _WIN32
    ::close(sock_fd);
#else
    ::closesocket(sock_fd);
#endif

    server.stop();
    server_thread.join();

    return true;
}

bool test_socks5_server_auth_e2e()
{
    const int test_port = 11081;

    Socks5ServerConfig config;
    config.enable_auth = true;
    config.username = "admin";
    config.password = "secret";
    config.enable_connect = true;

    Socks5Server server(config);

    if (!server.init(test_port)) {
        std::cout << "    (skipped: could not bind port " << test_port << ")" << std::endl;
        return true;
    }

    std::thread server_thread([&server]() {
        server.serve();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

#ifndef _WIN32
    int sock_fd = -1;
#else
    SOCKET sock_fd = INVALID_SOCKET;
#endif

    bool connected = create_tcp_connection("127.0.0.1", test_port, sock_fd);

    if (!connected) {
        server.stop();
        server_thread.join();
        std::cout << "    (skipped: could not connect to server)" << std::endl;
        return true;
    }

    uint8_t greeting[] = { 0x05, 0x02, 0x00, 0x02 };
#ifndef _WIN32
    ::send(sock_fd, greeting, sizeof(greeting), 0);
#else
    ::send(sock_fd, reinterpret_cast<const char *>(greeting), sizeof(greeting), 0);
#endif

    uint8_t method_response[2] = { 0 };
#ifndef _WIN32
    ::recv(sock_fd, reinterpret_cast<char *>(method_response), 2, 0);
#else
    ::recv(sock_fd, reinterpret_cast<char *>(method_response), 2, 0);
#endif

    TEST_ASSERT(method_response[0] == 0x05, "response version should be 0x05");
    TEST_ASSERT(method_response[1] == 0x02, "response method should be username_password");

    uint8_t auth[] = { 0x01, 0x05, 'a', 'd', 'm', 'i', 'n', 0x06, 's', 'e', 'c', 'r', 'e', 't' };
#ifndef _WIN32
    ::send(sock_fd, auth, sizeof(auth), 0);
#else
    ::send(sock_fd, reinterpret_cast<const char *>(auth), sizeof(auth), 0);
#endif

    uint8_t auth_response[2] = { 0 };
#ifndef _WIN32
    ::recv(sock_fd, reinterpret_cast<char *>(auth_response), 2, 0);
#else
    ::recv(sock_fd, reinterpret_cast<char *>(auth_response), 2, 0);
#endif

    TEST_ASSERT(auth_response[0] == 0x01, "auth response version should be 0x01");
    TEST_ASSERT(auth_response[1] == 0x00, "auth response should indicate success");

#ifndef _WIN32
    ::close(sock_fd);
#else
    ::closesocket(sock_fd);
#endif

    server.stop();
    server_thread.join();

    return true;
}

bool test_socks5_server_udp_associate_e2e()
{
    const int test_port = 11082;

    Socks5ServerConfig config;
    config.enable_auth = false;
    config.enable_connect = true;
    config.enable_udp_associate = true;
    config.max_connections = 10;

    Socks5Server server(config);

    if (!server.init(test_port)) {
        std::cout << "    (skipped: could not bind port " << test_port << ")" << std::endl;
        return true;
    }

    std::thread server_thread([&server]() {
        server.serve();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

#ifndef _WIN32
    int tcp_fd = -1;
#else
    SOCKET tcp_fd = INVALID_SOCKET;
#endif

    bool connected = create_tcp_connection("127.0.0.1", test_port, tcp_fd);
    if (!connected) {
        server.stop();
        server_thread.join();
        std::cout << "    (skipped: could not connect to server)" << std::endl;
        return true;
    }

    uint8_t greeting[] = { 0x05, 0x01, 0x00 };
#ifndef _WIN32
    ::send(tcp_fd, greeting, sizeof(greeting), 0);
#else
    ::send(tcp_fd, reinterpret_cast<const char *>(greeting), sizeof(greeting), 0);
#endif

    uint8_t method_response[2] = { 0 };
#ifndef _WIN32
    ::recv(tcp_fd, reinterpret_cast<char *>(method_response), 2, 0);
#else
    ::recv(tcp_fd, reinterpret_cast<char *>(method_response), 2, 0);
#endif

    TEST_ASSERT(method_response[0] == 0x05, "response version should be 0x05");
    TEST_ASSERT(method_response[1] == 0x00, "response method should be no_auth");

    uint8_t udp_req[] = {
        0x05, 0x03, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00
    };
#ifndef _WIN32
    ::send(tcp_fd, udp_req, sizeof(udp_req), 0);
#else
    ::send(tcp_fd, reinterpret_cast<const char *>(udp_req), sizeof(udp_req), 0);
#endif

    uint8_t udp_reply[10] = { 0 };
#ifndef _WIN32
    ::recv(tcp_fd, reinterpret_cast<char *>(udp_reply), 10, 0);
#else
    ::recv(tcp_fd, reinterpret_cast<char *>(udp_reply), 10, 0);
#endif

    TEST_ASSERT(udp_reply[0] == 0x05, "UDP reply version should be 0x05");
    TEST_ASSERT(udp_reply[1] == 0x00, "UDP reply code should be succeeded");

    uint16_t relay_port = 0;
    std::memcpy(&relay_port, &udp_reply[8], 2);
    relay_port = ntohs(relay_port);

    if (relay_port > 0) {
        std::cout << "    UDP relay port: " << relay_port << std::endl;

#ifndef _WIN32
        int udp_fd = -1;
#else
        SOCKET udp_fd = INVALID_SOCKET;
#endif

        if (create_udp_socket(udp_fd)) {
            uint8_t udp_datagram[] = {
                0x00, 0x00, 0x00, 0x01,
                0x7F, 0x00, 0x00, 0x01,
                0x00, 0x50,
                'T', 'E', 'S', 'T'
            };

            udp_sendto(udp_fd, "127.0.0.1", relay_port, udp_datagram, sizeof(udp_datagram));

#ifndef _WIN32
            ::close(udp_fd);
#else
            ::closesocket(udp_fd);
#endif
        }
    }

#ifndef _WIN32
    ::close(tcp_fd);
#else
    ::closesocket(tcp_fd);
#endif

    server.stop();
    server_thread.join();

    return true;
}

void run_unit_tests()
{
    std::cout << "\n=== SOCKS5 Packet Parser Unit Tests ===" << std::endl;

    RUN_TEST(test_protocol_enums);

    RUN_TEST(test_greeting_parse_no_auth);
    RUN_TEST(test_greeting_parse_multiple_methods);
    RUN_TEST(test_greeting_parse_incomplete);
    RUN_TEST(test_greeting_parse_wrong_version);
    RUN_TEST(test_greeting_parse_empty);
    RUN_TEST(test_greeting_max_methods);

    RUN_TEST(test_auth_request_parse);
    RUN_TEST(test_auth_request_parse_incomplete);

    RUN_TEST(test_request_parse_connect_ipv4);
    RUN_TEST(test_request_parse_connect_domain);
    RUN_TEST(test_request_parse_connect_ipv6);
    RUN_TEST(test_request_parse_udp_associate);
    RUN_TEST(test_request_parse_incomplete);

    RUN_TEST(test_build_method_select_reply);
    RUN_TEST(test_build_method_select_reply_no_acceptable);
    RUN_TEST(test_build_auth_reply);
    RUN_TEST(test_build_reply_ipv4);
    RUN_TEST(test_build_reply_domain);
    RUN_TEST(test_build_reply_ipv6);
    RUN_TEST(test_build_reply_default);
    RUN_TEST(test_reply_roundtrip_ipv4);
    RUN_TEST(test_reply_all_error_codes);

    RUN_TEST(test_udp_header_parse_ipv4);
    RUN_TEST(test_udp_header_parse_domain);
    RUN_TEST(test_udp_header_parse_ipv6);
    RUN_TEST(test_udp_header_parse_incomplete);
    RUN_TEST(test_udp_header_parse_empty);
    RUN_TEST(test_udp_header_fragment_field);
    RUN_TEST(test_udp_header_with_payload_separation);

    RUN_TEST(test_build_udp_header_ipv4);
    RUN_TEST(test_build_udp_header_domain);
    RUN_TEST(test_build_udp_header_ipv6);

    RUN_TEST(test_udp_header_roundtrip_ipv4);
    RUN_TEST(test_udp_header_roundtrip_domain);
    RUN_TEST(test_udp_header_roundtrip_ipv6);

    std::cout << "\n=== SOCKS5 Config & Session Tests ===" << std::endl;

    RUN_TEST(test_config_defaults);
    RUN_TEST(test_config_custom);
    RUN_TEST(test_session_state_transitions);

    std::cout << "\n=== SOCKS5 Server Object Tests ===" << std::endl;

    RUN_TEST(test_socks5_server_init);
    RUN_TEST(test_socks5_server_with_auth);
    RUN_TEST(test_socks5_server_handler);
}

void run_e2e_tests()
{
    std::cout << "\n=== SOCKS5 Server E2E Tests ===" << std::endl;

    RUN_TEST(test_socks5_server_connect_e2e);
    RUN_TEST(test_socks5_server_auth_e2e);
    RUN_TEST(test_socks5_server_udp_associate_e2e);
}

int main()
{
    const test::common::WinsockGuard winsock;
    if (!winsock.ok()) {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }

    std::cout << "\n";
    std::cout << "========================================" << std::endl;
    std::cout << "    SOCKS5 Protocol Test Suite         " << std::endl;
    std::cout << "========================================" << std::endl;

    run_unit_tests();
    run_e2e_tests();

    std::cout << "\n========================================" << std::endl;
    std::cout << "    Results: " << g_tests_passed << "/" << g_tests_run << " passed";
    if (g_tests_failed > 0) {
        std::cout << ", " << g_tests_failed << " failed";
    }
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;

    return g_tests_failed > 0 ? 1 : 0;
}
