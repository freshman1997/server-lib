#include "shadowsocks.h"

#include "buffer/byte_buffer.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace yuan::net::shadowsocks;
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

bool test_method_parse_and_spec()
{
    auto m1 = parse_method("aes-128-gcm");
    TEST_ASSERT(m1.has_value(), "aes-128-gcm should be supported");
    auto m2 = parse_method("aes-256-gcm");
    TEST_ASSERT(m2.has_value(), "aes-256-gcm should be supported");
    auto m3 = parse_method("chacha20-ietf-poly1305");
    TEST_ASSERT(m3.has_value(), "chacha20-ietf-poly1305 should be supported");
    auto bad = parse_method("aes-256-cfb");
    TEST_ASSERT(!bad.has_value(), "aes-256-cfb should not be supported in MVP");

    const auto &spec = method_spec(*m3);
    TEST_ASSERT(std::string(spec.name) == "chacha20-ietf-poly1305", "method name should match");
    TEST_ASSERT(spec.key_size == 32, "chacha key size should be 32");
    TEST_ASSERT(spec.salt_size == 32, "chacha salt size should be 32");
    TEST_ASSERT(spec.nonce_size == 12, "nonce size should be 12");
    TEST_ASSERT(spec.tag_size == 16, "tag size should be 16");
    return true;
}

bool test_nonce_increment()
{
    std::vector<uint8_t> nonce(3, 0);
    TEST_ASSERT(ShadowsocksCrypto::increment_nonce(nonce), "increment should succeed");
    TEST_ASSERT(nonce[0] == 1 && nonce[1] == 0 && nonce[2] == 0, "little-endian increment");

    nonce[0] = 0xFF;
    nonce[1] = 0x00;
    nonce[2] = 0x00;
    TEST_ASSERT(ShadowsocksCrypto::increment_nonce(nonce), "carry increment should succeed");
    TEST_ASSERT(nonce[0] == 0x00 && nonce[1] == 0x01 && nonce[2] == 0x00, "carry should apply to next byte");
    return true;
}

bool test_derive_master_key()
{
    std::vector<uint8_t> key;
    TEST_ASSERT(!ShadowsocksCrypto::derive_master_key("", CipherMethod::aes_128_gcm, key), "empty password should fail");
    TEST_ASSERT(ShadowsocksCrypto::derive_master_key("secret", CipherMethod::aes_128_gcm, key), "derive key should succeed");
    TEST_ASSERT(key.size() == 16, "aes-128-gcm key should be 16 bytes");

    TEST_ASSERT(ShadowsocksCrypto::derive_master_key("secret", CipherMethod::aes_256_gcm, key), "derive key should succeed");
    TEST_ASSERT(key.size() == 32, "aes-256-gcm key should be 32 bytes");
    return true;
}

bool test_derive_subkey_and_aead_roundtrip()
{
    auto method_opt = parse_method("chacha20-ietf-poly1305");
    TEST_ASSERT(method_opt.has_value(), "method should parse");
    const auto method = *method_opt;
    const auto &spec = method_spec(method);

    std::vector<uint8_t> master_key;
    TEST_ASSERT(ShadowsocksCrypto::derive_master_key("secret", method, master_key), "master key derivation should succeed");
    TEST_ASSERT(master_key.size() == spec.key_size, "master key size should match");

    std::vector<uint8_t> salt(spec.salt_size, 0x11);
    std::vector<uint8_t> subkey;
    TEST_ASSERT(ShadowsocksCrypto::derive_subkey(master_key, method, salt.data(), salt.size(), subkey), "subkey derivation should succeed");
    TEST_ASSERT(subkey.size() == spec.key_size, "subkey size should match");

    std::vector<uint8_t> nonce(spec.nonce_size, 0);
    std::string plain_text = "hello-shadowsocks";
    std::vector<uint8_t> encrypted;
    TEST_ASSERT(ShadowsocksCrypto::aead_encrypt(method,
                                                subkey,
                                                nonce,
                                                reinterpret_cast<const uint8_t *>(plain_text.data()),
                                                plain_text.size(),
                                                nullptr,
                                                0,
                                                encrypted),
                "aead encrypt should succeed");
    TEST_ASSERT(encrypted.size() == plain_text.size() + spec.tag_size, "ciphertext+tag size should match");

    std::vector<uint8_t> decrypted;
    TEST_ASSERT(ShadowsocksCrypto::aead_decrypt(method,
                                                subkey,
                                                nonce,
                                                encrypted.data(),
                                                encrypted.size(),
                                                nullptr,
                                                0,
                                                decrypted),
                "aead decrypt should succeed");
    TEST_ASSERT(decrypted.size() == plain_text.size(), "decrypted size should match plaintext");
    TEST_ASSERT(std::string(reinterpret_cast<const char *>(decrypted.data()), decrypted.size()) == plain_text,
                "decrypted plaintext should match original");
    return true;
}

bool test_aead_bad_tag_should_fail()
{
    auto method_opt = parse_method("aes-128-gcm");
    TEST_ASSERT(method_opt.has_value(), "method should parse");
    const auto method = *method_opt;
    const auto &spec = method_spec(method);

    std::vector<uint8_t> master_key;
    TEST_ASSERT(ShadowsocksCrypto::derive_master_key("secret", method, master_key), "master key derivation should succeed");

    std::vector<uint8_t> salt(spec.salt_size, 0x22);
    std::vector<uint8_t> subkey;
    TEST_ASSERT(ShadowsocksCrypto::derive_subkey(master_key, method, salt.data(), salt.size(), subkey), "subkey derivation should succeed");

    std::vector<uint8_t> nonce(spec.nonce_size, 0);
    const std::string plain_text = "payload";
    std::vector<uint8_t> encrypted;
    TEST_ASSERT(ShadowsocksCrypto::aead_encrypt(method,
                                                subkey,
                                                nonce,
                                                reinterpret_cast<const uint8_t *>(plain_text.data()),
                                                plain_text.size(),
                                                nullptr,
                                                0,
                                                encrypted),
                "aead encrypt should succeed");

    TEST_ASSERT(!encrypted.empty(), "encrypted buffer should not be empty");
    encrypted.back() ^= 0x01;

    std::vector<uint8_t> decrypted;
    TEST_ASSERT(!ShadowsocksCrypto::aead_decrypt(method,
                                                 subkey,
                                                 nonce,
                                                 encrypted.data(),
                                                 encrypted.size(),
                                                 nullptr,
                                                 0,
                                                 decrypted),
                "tampered tag should fail");
    return true;
}

bool test_target_address_codec_domain()
{
    ByteBuffer buf(64);
    TargetAddress target;
    target.atyp = AddressType::domain;
    target.host = "example.com";
    target.port = 443;

    TEST_ASSERT(ShadowsocksPacketCodec::append_target_address(buf, target), "append target should succeed");

    std::size_t consumed = 0;
    auto parsed = ShadowsocksPacketCodec::parse_target_address(buf, consumed);
    TEST_ASSERT(parsed.has_value(), "parse target should succeed");
    TEST_ASSERT(parsed->atyp == AddressType::domain, "atyp should be domain");
    TEST_ASSERT(parsed->host == "example.com", "host should match");
    TEST_ASSERT(parsed->port == 443, "port should match");
    TEST_ASSERT(consumed == buf.readable_bytes(), "consumed size should match packet size");
    return true;
}

bool test_target_address_codec_ipv4()
{
    ByteBuffer buf(64);
    TargetAddress target;
    target.atyp = AddressType::ipv4;
    target.host = "127.0.0.1";
    target.port = 8080;

    TEST_ASSERT(ShadowsocksPacketCodec::append_target_address(buf, target), "append ipv4 target should succeed");

    std::size_t consumed = 0;
    auto parsed = ShadowsocksPacketCodec::parse_target_address(buf, consumed);
    TEST_ASSERT(parsed.has_value(), "parse ipv4 target should succeed");
    TEST_ASSERT(parsed->atyp == AddressType::ipv4, "atyp should be ipv4");
    TEST_ASSERT(parsed->host == "127.0.0.1", "host should match");
    TEST_ASSERT(parsed->port == 8080, "port should match");
    TEST_ASSERT(consumed == buf.readable_bytes(), "consumed size should match packet size");
    return true;
}

bool test_target_address_codec_invalid_domain()
{
    ByteBuffer buf(300);
    TargetAddress target;
    target.atyp = AddressType::domain;
    target.host.assign(260, 'a');
    target.port = 53;

    TEST_ASSERT(!ShadowsocksPacketCodec::append_target_address(buf, target), "too long domain should fail");
    return true;
}

bool test_tcp_chunk_codec_roundtrip()
{
    auto method_opt = parse_method("chacha20-ietf-poly1305");
    TEST_ASSERT(method_opt.has_value(), "method should parse");
    const auto method = *method_opt;
    const auto &spec = method_spec(method);

    std::vector<uint8_t> master_key;
    TEST_ASSERT(ShadowsocksCrypto::derive_master_key("secret", method, master_key), "master key derivation should succeed");

    std::vector<uint8_t> salt(spec.salt_size, 0x33);
    std::vector<uint8_t> subkey;
    TEST_ASSERT(ShadowsocksCrypto::derive_subkey(master_key, method, salt.data(), salt.size(), subkey), "subkey derivation should succeed");

    std::vector<uint8_t> send_nonce(spec.nonce_size, 0);
    std::vector<uint8_t> recv_nonce(spec.nonce_size, 0);

    const std::string payload_text = "tcp-chunk-payload";
    ByteBuffer encrypted(128);
    TEST_ASSERT(ShadowsocksPacketCodec::append_tcp_chunk(encrypted,
                                                          method,
                                                          subkey,
                                                          send_nonce,
                                                          reinterpret_cast<const uint8_t *>(payload_text.data()),
                                                          payload_text.size()),
                "append tcp chunk should succeed");

    auto span = encrypted.readable_span();
    auto parsed = ShadowsocksPacketCodec::try_parse_tcp_chunk(
        reinterpret_cast<const uint8_t *>(span.data()),
        span.size(),
        method,
        subkey,
        recv_nonce);

    TEST_ASSERT(parsed.complete, "chunk parse should complete");
    TEST_ASSERT(!parsed.malformed, "chunk parse should not be malformed");
    TEST_ASSERT(parsed.consumed == span.size(), "consumed bytes should match chunk size");
    TEST_ASSERT(parsed.plaintext.size() == payload_text.size(), "decoded payload size should match");
    TEST_ASSERT(std::string(reinterpret_cast<const char *>(parsed.plaintext.data()), parsed.plaintext.size()) == payload_text,
                "decoded payload should match input");
    return true;
}

bool test_tcp_chunk_codec_incomplete_and_tampered()
{
    auto method_opt = parse_method("aes-256-gcm");
    TEST_ASSERT(method_opt.has_value(), "method should parse");
    const auto method = *method_opt;
    const auto &spec = method_spec(method);

    std::vector<uint8_t> master_key;
    TEST_ASSERT(ShadowsocksCrypto::derive_master_key("secret", method, master_key), "master key derivation should succeed");
    std::vector<uint8_t> salt(spec.salt_size, 0x44);
    std::vector<uint8_t> subkey;
    TEST_ASSERT(ShadowsocksCrypto::derive_subkey(master_key, method, salt.data(), salt.size(), subkey), "subkey derivation should succeed");

    std::vector<uint8_t> send_nonce(spec.nonce_size, 0);
    ByteBuffer encrypted(128);
    const std::string payload_text = "abc";
    TEST_ASSERT(ShadowsocksPacketCodec::append_tcp_chunk(encrypted,
                                                          method,
                                                          subkey,
                                                          send_nonce,
                                                          reinterpret_cast<const uint8_t *>(payload_text.data()),
                                                          payload_text.size()),
                "append tcp chunk should succeed");

    auto span = encrypted.readable_span();
    std::vector<uint8_t> recv_nonce_incomplete(spec.nonce_size, 0);
    auto parsed_incomplete = ShadowsocksPacketCodec::try_parse_tcp_chunk(
        reinterpret_cast<const uint8_t *>(span.data()),
        1,
        method,
        subkey,
        recv_nonce_incomplete);
    TEST_ASSERT(!parsed_incomplete.complete, "incomplete chunk should not complete");
    TEST_ASSERT(!parsed_incomplete.malformed, "incomplete chunk should not be malformed");

    std::vector<uint8_t> tampered(span.begin(), span.end());
    TEST_ASSERT(!tampered.empty(), "tampered buffer should not be empty");
    tampered.back() ^= 0x7F;

    std::vector<uint8_t> recv_nonce_tampered(spec.nonce_size, 0);
    auto parsed_tampered = ShadowsocksPacketCodec::try_parse_tcp_chunk(
        tampered.data(),
        tampered.size(),
        method,
        subkey,
        recv_nonce_tampered);
    TEST_ASSERT(!parsed_tampered.complete, "tampered chunk should not complete");
    TEST_ASSERT(parsed_tampered.malformed, "tampered chunk should be malformed");
    return true;
}

bool test_udp_packet_codec_roundtrip()
{
    auto method_opt = parse_method("chacha20-ietf-poly1305");
    TEST_ASSERT(method_opt.has_value(), "method should parse");
    const auto method = *method_opt;

    std::vector<uint8_t> master_key;
    TEST_ASSERT(ShadowsocksCrypto::derive_master_key("secret", method, master_key), "master key derivation should succeed");

    ByteBuffer plain(128);
    TargetAddress target;
    target.atyp = AddressType::domain;
    target.host = "example.com";
    target.port = 53;
    TEST_ASSERT(ShadowsocksPacketCodec::append_target_address(plain, target), "append target should succeed");
    const std::string body = "dns-payload";
    plain.append(reinterpret_cast<const uint8_t *>(body.data()), body.size());

    ByteBuffer udp_packet(256);
    auto span = plain.readable_span();
    TEST_ASSERT(ShadowsocksPacketCodec::append_udp_packet(udp_packet,
                                                           method,
                                                           master_key,
                                                           reinterpret_cast<const uint8_t *>(span.data()),
                                                           span.size()),
                "append udp packet should succeed");

    auto enc = udp_packet.readable_span();
    auto decoded = ShadowsocksPacketCodec::parse_udp_packet(
        reinterpret_cast<const uint8_t *>(enc.data()),
        enc.size(),
        method,
        master_key);

    TEST_ASSERT(decoded.complete, "decoded udp packet should be complete");
    TEST_ASSERT(!decoded.malformed, "decoded udp packet should not be malformed");
    TEST_ASSERT(decoded.plaintext.size() == span.size(), "decoded size should match original");
    TEST_ASSERT(std::memcmp(decoded.plaintext.data(), span.data(), span.size()) == 0,
                "decoded plaintext should match original");
    return true;
}

bool test_udp_packet_codec_tampered_should_fail()
{
    auto method_opt = parse_method("aes-256-gcm");
    TEST_ASSERT(method_opt.has_value(), "method should parse");
    const auto method = *method_opt;

    std::vector<uint8_t> master_key;
    TEST_ASSERT(ShadowsocksCrypto::derive_master_key("secret", method, master_key), "master key derivation should succeed");

    const std::string body = "udp-body";
    ByteBuffer udp_packet(128);
    TEST_ASSERT(ShadowsocksPacketCodec::append_udp_packet(udp_packet,
                                                           method,
                                                           master_key,
                                                           reinterpret_cast<const uint8_t *>(body.data()),
                                                           body.size()),
                "append udp packet should succeed");

    auto enc = udp_packet.readable_span();
    std::vector<uint8_t> tampered(enc.begin(), enc.end());
    TEST_ASSERT(!tampered.empty(), "tampered packet should not be empty");
    tampered.back() ^= 0x55;

    auto decoded = ShadowsocksPacketCodec::parse_udp_packet(
        tampered.data(),
        tampered.size(),
        method,
        master_key);

    TEST_ASSERT(!decoded.complete, "tampered udp packet should not complete");
    TEST_ASSERT(decoded.malformed, "tampered udp packet should be malformed");
    return true;
}

int main()
{
    std::cout << "=== Shadowsocks protocol tests ===" << std::endl;

    RUN_TEST(test_method_parse_and_spec);
    RUN_TEST(test_nonce_increment);
    RUN_TEST(test_derive_master_key);
    RUN_TEST(test_derive_subkey_and_aead_roundtrip);
    RUN_TEST(test_aead_bad_tag_should_fail);
    RUN_TEST(test_target_address_codec_domain);
    RUN_TEST(test_target_address_codec_ipv4);
    RUN_TEST(test_target_address_codec_invalid_domain);
    RUN_TEST(test_tcp_chunk_codec_roundtrip);
    RUN_TEST(test_tcp_chunk_codec_incomplete_and_tampered);
    RUN_TEST(test_udp_packet_codec_roundtrip);
    RUN_TEST(test_udp_packet_codec_tampered_should_fail);

    std::cout << "\n=== Test Summary ===" << std::endl;
    std::cout << "Total:  " << g_tests_run << std::endl;
    std::cout << "Passed: " << g_tests_passed << std::endl;
    std::cout << "Failed: " << g_tests_failed << std::endl;

    if (g_tests_failed == 0) {
        std::cout << "\nAll tests passed!" << std::endl;
        return 0;
    }

    std::cout << "\nSome tests failed." << std::endl;
    return 1;
}
