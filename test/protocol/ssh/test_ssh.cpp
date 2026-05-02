#include "ssh.h"

#include "openssl/core_names.h"
#include "openssl/evp.h"
#include "openssl/params.h"
#include "openssl/x509.h"

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace yuan::net::ssh;

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

namespace
{
    class FakeKexAlgorithm final : public SshKexAlgorithm
    {
    public:
        std::string name() const override
        {
            return "test-kex";
        }

        size_t hash_digest_size() const override
        {
            return 32;
        }

        std::vector<uint8_t> generate_public_key() override
        {
            return { 0xAA, 0xBB, 0xCC };
        }

        bool compute_shared_secret(const std::vector<uint8_t> & peer_public,
                                   std::vector<uint8_t> & shared_secret) override
        {
            shared_secret = peer_public;
            shared_secret.push_back(0x42);
            return true;
        }

        std::vector<uint8_t> compute_exchange_hash(
            const std::string & client_version,
            const std::string & server_version,
            const std::vector<uint8_t> & client_kex_init,
            const std::vector<uint8_t> & server_kex_init,
            const std::vector<uint8_t> & host_key,
            const std::vector<uint8_t> & client_public,
            const std::vector<uint8_t> & server_public,
            const std::vector<uint8_t> & shared_secret) override
        {
            std::vector<uint8_t> result;
            result.push_back(static_cast<uint8_t>(client_version.size()));
            result.push_back(static_cast<uint8_t>(server_version.size()));
            result.push_back(static_cast<uint8_t>(client_kex_init.size()));
            result.push_back(static_cast<uint8_t>(server_kex_init.size()));
            result.push_back(static_cast<uint8_t>(host_key.size()));
            result.push_back(static_cast<uint8_t>(client_public.size()));
            result.push_back(static_cast<uint8_t>(server_public.size()));
            result.push_back(static_cast<uint8_t>(shared_secret.size()));
            return result;
        }

        std::vector<uint8_t> public_key() const override
        {
            return { 0xAA, 0xBB, 0xCC };
        }
    };

    class FakeHostKeyAlgorithm final : public SshHostKeyAlgorithm
    {
    public:
        std::string name() const override
        {
            return "test-host";
        }

        std::vector<uint8_t> public_key_blob() const override
        {
            return { 0x10, 0x20, 0x30 };
        }

        std::vector<uint8_t> sign(const std::vector<uint8_t> & data) override
        {
            std::vector<uint8_t> signature = { 0x99 };
            signature.insert(signature.end(), data.begin(), data.end());
            return signature;
        }

        bool verify(const std::vector<uint8_t> & data,
                    const std::vector<uint8_t> & signature) override
        {
            return signature.size() == data.size() + 1 && signature[0] == 0x99;
        }

        std::string fingerprint() const override
        {
            return "fake";
        }
    };

    class FakeCipher final : public SshCipher
    {
    public:
        std::string name() const override
        {
            return "test-cipher";
        }

        size_t block_size() const override
        {
            return 8;
        }

        size_t key_size() const override
        {
            return 16;
        }

        size_t iv_size() const override
        {
            return 16;
        }

        void init(const std::vector<uint8_t> & key,
                  const std::vector<uint8_t> & iv) override
        {
            initialized_ = !key.empty() && !iv.empty();
        }

        std::vector<uint8_t> encrypt(const uint8_t * data, size_t len) override
        {
            return std::vector<uint8_t>(data, data + len);
        }

        std::vector<uint8_t> decrypt(const uint8_t * data, size_t len) override
        {
            return std::vector<uint8_t>(data, data + len);
        }

        bool decrypt_length(const uint8_t * enc_length, size_t enc_length_len,
                            const uint8_t * seq_bytes,
                            uint8_t * out_length) const override
        {
            (void)seq_bytes;
            if (enc_length_len < 4 || !out_length) {
                return false;
            }
            for (size_t i = 0; i < 4; ++i) {
                out_length[i] = enc_length[i];
            }
            return true;
        }

        bool initialized() const
        {
            return initialized_;
        }

    private:
        bool initialized_ = false;
    };

    class FakeMac final : public SshMac
    {
    public:
        std::string name() const override
        {
            return "test-mac";
        }

        size_t digest_size() const override
        {
            return 4;
        }

        size_t key_size() const override
        {
            return 16;
        }

        void init(const std::vector<uint8_t> & key) override
        {
            initialized_ = !key.empty();
        }

        std::vector<uint8_t> compute(uint32_t seq,
                                     const uint8_t * data, size_t len) override
        {
            (void)seq;
            (void)data;
            (void)len;
            return { 0x01, 0x02, 0x03, 0x04 };
        }

        bool verify(uint32_t seq,
                    const uint8_t * data, size_t len,
                    const uint8_t * mac, size_t mac_len) override
        {
            (void)seq;
            (void)data;
            (void)len;
            return mac_len == 4 &&
                   mac[0] == 0x01 &&
                   mac[1] == 0x02 &&
                   mac[2] == 0x03 &&
                   mac[3] == 0x04;
        }

    private:
        bool initialized_ = false;
    };

    class FakeCompression final : public SshCompression
    {
    public:
        std::string name() const override
        {
            return "none";
        }

        bool init() override
        {
            return true;
        }

        std::vector<uint8_t> compress(const uint8_t * data, size_t len) override
        {
            return std::vector<uint8_t>(data, data + len);
        }

        std::vector<uint8_t> decompress(const uint8_t * data, size_t len) override
        {
            return std::vector<uint8_t>(data, data + len);
        }
    };

    class FakeAeadCipher final : public SshCipher
    {
    public:
        std::string name() const override
        {
            return "test-aead";
        }

        size_t block_size() const override
        {
            return 8;
        }

        size_t key_size() const override
        {
            return 16;
        }

        size_t iv_size() const override
        {
            return 12;
        }

        size_t tag_size() const override
        {
            return 16;
        }

        void init(const std::vector<uint8_t> & key,
                  const std::vector<uint8_t> & iv) override
        {
            initialized_ = !key.empty() && !iv.empty();
        }

        std::vector<uint8_t> encrypt(const uint8_t * data, size_t len) override
        {
            return std::vector<uint8_t>(data, data + len);
        }

        std::vector<uint8_t> decrypt(const uint8_t * data, size_t len) override
        {
            return std::vector<uint8_t>(data, data + len);
        }

        bool is_aead() const override
        {
            return true;
        }

        std::vector<uint8_t> encrypt_aead(const uint8_t * aad, size_t aad_len,
                                          const uint8_t * data, size_t data_len,
                                          const uint8_t * seq_bytes) override
        {
            (void)seq_bytes;
            std::vector<uint8_t> result(data, data + data_len);
            uint8_t checksum = 0;
            for (size_t i = 0; i < aad_len; ++i) {
                checksum ^= aad[i];
            }
            for (size_t i = 0; i < data_len; ++i) {
                checksum ^= data[i];
            }
            result.resize(data_len + tag_size(), checksum);
            return result;
        }

        bool decrypt_aead(const uint8_t * aad, size_t aad_len,
                          const uint8_t * data, size_t data_len,
                          const uint8_t * tag, size_t tag_len,
                          const uint8_t * seq_bytes,
                          std::vector<uint8_t> & out) override
        {
            (void)seq_bytes;
            if (tag_len != tag_size()) {
                return false;
            }

            uint8_t checksum = 0;
            for (size_t i = 0; i < aad_len; ++i) {
                checksum ^= aad[i];
            }
            for (size_t i = 0; i < data_len; ++i) {
                checksum ^= data[i];
            }
            for (size_t i = 0; i < tag_len; ++i) {
                if (tag[i] != checksum) {
                    return false;
                }
            }

            out.assign(data, data + data_len);
            return true;
        }

    private:
        bool initialized_ = false;
    };

    class FakeOpaqueCipher final : public SshCipher
    {
    public:
        std::string name() const override
        {
            return "test-opaque";
        }

        size_t block_size() const override
        {
            return 8;
        }

        size_t key_size() const override
        {
            return 16;
        }

        size_t iv_size() const override
        {
            return 16;
        }

        void init(const std::vector<uint8_t> & key,
                  const std::vector<uint8_t> & iv) override
        {
            initialized_ = !key.empty() && !iv.empty();
        }

        std::vector<uint8_t> encrypt(const uint8_t * data, size_t len) override
        {
            std::vector<uint8_t> out(data, data + len);
            for (auto & byte : out) {
                byte ^= 0xA5;
            }
            return out;
        }

        std::vector<uint8_t> decrypt(const uint8_t * data, size_t len) override
        {
            return encrypt(data, len);
        }

        bool decrypt_length(const uint8_t * enc_length, size_t enc_length_len,
                            const uint8_t * seq_bytes,
                            uint8_t * out_length) const override
        {
            (void)seq_bytes;
            if (enc_length_len < 4 || !out_length) {
                return false;
            }
            for (size_t i = 0; i < 4; ++i) {
                out_length[i] = static_cast<uint8_t>(enc_length[i] ^ 0xA5);
            }
            return true;
        }

    private:
        bool initialized_ = false;
    };

    class RecordingSubsystem final : public SshChannelHandler
    {
    public:
        explicit RecordingSubsystem(bool * opened)
            : opened_(opened)
        {
        }

        void on_open(SshChannel * channel) override
        {
            (void)channel;
            if (opened_) {
                *opened_ = true;
            }
        }

    private:
        bool * opened_;
    };

    std::vector<uint8_t> encode_string_payload(const std::string & value)
    {
        yuan::buffer::ByteBuffer buf;
        SshMessageCodec::write_string(buf, value);
        auto span = buf.readable_span();
        return std::vector<uint8_t>(span.data(), span.data() + span.size());
    }

    std::vector<uint8_t> build_userauth_signed_data(const std::vector<uint8_t> & session_id,
                                                    const std::string & username,
                                                    const std::string & algorithm,
                                                    const std::vector<uint8_t> & public_key_blob)
    {
        yuan::buffer::ByteBuffer signed_data;
        SshMessageCodec::write_raw(signed_data, session_id.data(), session_id.size());
        signed_data.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_REQUEST));
        SshMessageCodec::write_string(signed_data, username);
        SshMessageCodec::write_string(signed_data, SSH_SERVICE_CONNECTION);
        SshMessageCodec::write_string(signed_data, "publickey");
        SshMessageCodec::write_boolean(signed_data, true);
        SshMessageCodec::write_string(signed_data, algorithm);
        SshMessageCodec::write_raw(signed_data, public_key_blob.data(), public_key_blob.size());

        auto span = signed_data.readable_span();
        return std::vector<uint8_t>(
            reinterpret_cast<const uint8_t *>(span.data()),
            reinterpret_cast<const uint8_t *>(span.data()) + span.size());
    }

    std::vector<uint8_t> wrap_signature_blob(const std::string & algorithm, const std::vector<uint8_t> & signature)
    {
        yuan::buffer::ByteBuffer inner;
        SshMessageCodec::write_string(inner, algorithm);
        SshMessageCodec::write_string(inner, std::string(
            reinterpret_cast<const char *>(signature.data()),
            signature.size()));

        auto inner_span = inner.readable_span();
        yuan::buffer::ByteBuffer outer;
        SshMessageCodec::write_string(outer, std::string(inner_span.data(), inner_span.size()));

        auto outer_span = outer.readable_span();
        return std::vector<uint8_t>(
            reinterpret_cast<const uint8_t *>(outer_span.data()),
            reinterpret_cast<const uint8_t *>(outer_span.data()) + outer_span.size());
    }

    std::vector<uint8_t> build_rsa_public_key_blob(const std::vector<uint8_t> & public_key_der)
    {
        const uint8_t * der_ptr = public_key_der.data();
        EVP_PKEY * pkey = d2i_PUBKEY(nullptr, &der_ptr, static_cast<long>(public_key_der.size()));
        if (!pkey) {
            return {};
        }

        BIGNUM * n = nullptr;
        BIGNUM * e = nullptr;
        EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n);
        EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e);

        std::vector<uint8_t> modulus(BN_num_bytes(n));
        std::vector<uint8_t> exponent(BN_num_bytes(e));
        BN_bn2bin(n, modulus.data());
        BN_bn2bin(e, exponent.data());

        yuan::buffer::ByteBuffer buf;
        SshMessageCodec::write_string(buf, "ssh-rsa");
        SshMessageCodec::write_mpint(buf, exponent);
        SshMessageCodec::write_mpint(buf, modulus);

        BN_free(n);
        BN_free(e);
        EVP_PKEY_free(pkey);

        auto span = buf.readable_span();
        return std::vector<uint8_t>(
            reinterpret_cast<const uint8_t *>(span.data()),
            reinterpret_cast<const uint8_t *>(span.data()) + span.size());
    }

    std::vector<uint8_t> build_ecdsa_public_key_blob(const std::vector<uint8_t> & public_key_der,
                                                      const std::string & algorithm,
                                                      const std::string & curve_name)
    {
        const uint8_t * der_ptr = public_key_der.data();
        EVP_PKEY * pkey = d2i_PUBKEY(nullptr, &der_ptr, static_cast<long>(public_key_der.size()));
        if (!pkey) {
            return {};
        }

        size_t pub_len = 0;
        EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PUB_KEY, nullptr, 0, &pub_len);
        std::vector<uint8_t> public_point(pub_len);
        EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PUB_KEY, public_point.data(), pub_len, &pub_len);
        public_point.resize(pub_len);

        yuan::buffer::ByteBuffer buf;
        SshMessageCodec::write_string(buf, algorithm);
        SshMessageCodec::write_string(buf, curve_name);
        SshMessageCodec::write_string(buf, std::string(
            reinterpret_cast<const char *>(public_point.data()),
            public_point.size()));

        EVP_PKEY_free(pkey);

        auto span = buf.readable_span();
        return std::vector<uint8_t>(
            reinterpret_cast<const uint8_t *>(span.data()),
            reinterpret_cast<const uint8_t *>(span.data()) + span.size());
    }

    SshCipherContext build_active_cipher_context(SshAlgorithmRegistry & registry,
                                                 const std::string & cipher_name,
                                                 bool with_mac = true)
    {
        SshCipherContext ctx;
        SshNegotiatedAlgorithms negotiated;
        negotiated.kex_name = "test-kex";
        negotiated.kex_hash_name = "sha256";
        negotiated.client_to_server_cipher_name = cipher_name;
        negotiated.server_to_client_cipher_name = cipher_name;
        negotiated.client_to_server_mac_name = with_mac ? "test-mac" : "";
        negotiated.server_to_client_mac_name = with_mac ? "test-mac" : "";
        negotiated.client_to_server_compression_name = "none";
        negotiated.server_to_client_compression_name = "none";
        ctx.activate(negotiated, { 0x01, 0x02 }, { 0x03, 0x04 }, { 0x05, 0x06 }, true, &registry);
        return ctx;
    }

    std::filesystem::path make_temp_ssh_dir()
    {
        const auto dir = std::filesystem::temp_directory_path() /
                         std::filesystem::path("yuan_ssh_test_" + std::to_string(g_tests_run));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
}

bool test_password_auth_without_handler_uses_method_fallback()
{
    SshAuthenticator auth;
    auth.register_method(std::make_unique<SshAuthPassword>());
    TEST_ASSERT(auth.process_service_request(SSH_SERVICE_USERAUTH),
                "userauth service request should succeed");

    SshUserauthRequestMessage msg;
    msg.username = "demo";
    msg.service_name = SSH_SERVICE_CONNECTION;
    msg.method_name = "password";
    msg.method_specific_data.push_back(0);
    msg.method_specific_data.push_back(0);
    msg.method_specific_data.push_back(0);
    msg.method_specific_data.push_back(0);
    msg.method_specific_data.push_back(4);
    msg.method_specific_data.insert(msg.method_specific_data.end(), { 'p', 'a', 's', 's' });

    auto result = auth.process_userauth_request(nullptr, nullptr, msg);
    TEST_ASSERT(result == SshAuthResult::SUCCESS,
                "password auth should fall back to built-in method when handler is absent");
    TEST_ASSERT(auth.authenticated(), "authenticator should enter authenticated state");
    return true;
}

bool test_session_channel_open_without_handler_is_allowed()
{
    SshConnectionManager mgr(nullptr);

    SshChannelOpenMessage msg;
    msg.channel_type = SSH_CHANNEL_SESSION;
    msg.sender_channel = 7;
    msg.initial_window_size = SSH_DEFAULT_WINDOW_SIZE;
    msg.maximum_packet_size = SSH_DEFAULT_MAX_PACKET_SIZE;

    auto response = mgr.handle_channel_open(msg, nullptr);
    auto span = response.readable_span();
    TEST_ASSERT(!span.empty(), "channel open should produce a response");

    auto decoded = SshMessageCodec::decode_channel_open_confirmation(
        reinterpret_cast<const uint8_t *>(span.data()), span.size());
    TEST_ASSERT(decoded.has_value(), "session channel should be confirmed without a handler");
    TEST_ASSERT(mgr.channel_count() == 1, "session channel should be tracked after confirmation");
    return true;
}

bool test_builtin_subsystem_request_without_handler_is_allowed()
{
    bool subsystem_opened = false;
    SshConnectionManager mgr(nullptr);
    mgr.register_subsystem("sftp", [&subsystem_opened]() -> std::unique_ptr<SshChannelHandler> {
        return std::make_unique<RecordingSubsystem>(&subsystem_opened);
    });

    auto * channel = mgr.create_channel(SSH_CHANNEL_SESSION, 11, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");

    SshChannelRequestMessage msg;
    msg.recipient_channel = 11;
    msg.request_type = "subsystem";
    msg.want_reply = true;
    msg.request_specific_data = encode_string_payload("sftp");

    auto response = mgr.handle_channel_request(msg, nullptr);
    auto span = response.readable_span();
    TEST_ASSERT(!span.empty(), "subsystem request should produce a response");
    TEST_ASSERT(span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_SUCCESS),
                "builtin subsystem should return channel success");
    TEST_ASSERT(subsystem_opened, "builtin subsystem handler should be opened");
    TEST_ASSERT(channel->handler() != nullptr, "channel should retain subsystem handler");
    return true;
}

bool test_default_handler_denies_direct_tcpip_channel_open()
{
    SshConnectionManager mgr(nullptr);

    SshChannelOpenMessage msg;
    msg.channel_type = SSH_CHANNEL_DIRECT_TCPIP;
    msg.sender_channel = 9;
    msg.initial_window_size = SSH_DEFAULT_WINDOW_SIZE;
    msg.maximum_packet_size = SSH_DEFAULT_MAX_PACKET_SIZE;
    msg.type_specific_data = encode_string_payload("127.0.0.1");
    msg.type_specific_data.push_back(0);
    msg.type_specific_data.push_back(0);
    msg.type_specific_data.push_back(0x1F);
    msg.type_specific_data.push_back(0x90);

    auto response = mgr.handle_channel_open(msg, &SshHandler::default_handler());
    auto span = response.readable_span();
    TEST_ASSERT(!span.empty(), "direct-tcpip open should produce a response");

    auto decoded = SshMessageCodec::decode_channel_open_failure(
        reinterpret_cast<const uint8_t *>(span.data()), span.size());
    TEST_ASSERT(decoded.has_value(), "default handler should reject direct-tcpip channel opens");
    return true;
}

bool test_default_handler_still_allows_builtin_subsystem()
{
    bool subsystem_opened = false;
    SshConnectionManager mgr(nullptr);
    mgr.register_subsystem("sftp", [&subsystem_opened]() -> std::unique_ptr<SshChannelHandler> {
        return std::make_unique<RecordingSubsystem>(&subsystem_opened);
    });

    auto * channel = mgr.create_channel(SSH_CHANNEL_SESSION, 12, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");

    SshChannelRequestMessage msg;
    msg.recipient_channel = 12;
    msg.request_type = "subsystem";
    msg.want_reply = true;
    msg.request_specific_data = encode_string_payload("sftp");

    auto response = mgr.handle_channel_request(msg, &SshHandler::default_handler());
    auto span = response.readable_span();
    TEST_ASSERT(!span.empty(), "subsystem request should produce a response");
    TEST_ASSERT(span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_SUCCESS),
                "default handler should not block builtin subsystem activation");
    TEST_ASSERT(subsystem_opened, "builtin subsystem should still open under default handler");
    return true;
}

bool test_request_success_encoding_uses_message_byte()
{
    SshConnectionManager mgr(nullptr);
    auto response = mgr.build_request_success({ 0x00, 0x00, 0x08, 0xAE });
    auto span = response.readable_span();
    TEST_ASSERT(span.size() == 5, "request-success should contain 1-byte type and raw payload");
    TEST_ASSERT(span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_REQUEST_SUCCESS),
                "first byte should be SSH_MSG_REQUEST_SUCCESS");
    TEST_ASSERT(static_cast<uint8_t>(span.data()[4]) == 0xAE, "payload bytes should be appended raw");
    return true;
}

bool test_publickey_rsa_fallback_verifies_signature()
{
    SshCryptoOpenSSL crypto;
    SshAuthPublickey auth(&crypto);

    const auto keypair = crypto.generate_rsa_key_pair(2048);
    const auto public_key_blob = build_rsa_public_key_blob(keypair.public_key);
    TEST_ASSERT(!public_key_blob.empty(), "rsa public key blob should be built");

    const std::vector<uint8_t> session_id = { 0x10, 0x20, 0x30, 0x40 };
    const std::string username = "demo";
    const std::string algorithm = "rsa-sha2-256";
    const auto signed_data = build_userauth_signed_data(session_id, username, algorithm, public_key_blob);
    const auto raw_signature = crypto.rsa_sign(keypair.private_key, "sha256", signed_data.data(), signed_data.size());
    TEST_ASSERT(!raw_signature.empty(), "rsa signature should be generated");
    TEST_ASSERT(crypto.rsa_verify(keypair.public_key, "sha256",
                                  signed_data.data(), signed_data.size(),
                                  raw_signature.data(), raw_signature.size()),
                "rsa signature should verify against the original DER public key");

    const auto signature = wrap_signature_blob(algorithm, raw_signature);
    TEST_ASSERT(auth.verify_signature(session_id, username, algorithm, public_key_blob, signature),
                "rsa-sha2-256 publickey fallback should verify a valid signature");
    return true;
}

bool test_publickey_ecdsa_fallback_verifies_signature()
{
    SshCryptoOpenSSL crypto;
    SshAuthPublickey auth(&crypto);

    const auto keypair = crypto.generate_ecdsa_key_pair("P-256");
    const auto public_key_blob = build_ecdsa_public_key_blob(
        keypair.public_key, "ecdsa-sha2-nistp256", "nistp256");
    TEST_ASSERT(!public_key_blob.empty(), "ecdsa public key blob should be built");

    const std::vector<uint8_t> session_id = { 0x01, 0x23, 0x45, 0x67 };
    const std::string username = "demo";
    const std::string algorithm = "ecdsa-sha2-nistp256";
    const auto signed_data = build_userauth_signed_data(session_id, username, algorithm, public_key_blob);
    const auto raw_signature = crypto.ecdsa_sign(keypair.private_key, "P-256", signed_data.data(), signed_data.size());
    TEST_ASSERT(!raw_signature.empty(), "ecdsa signature should be generated");

    const auto signature = wrap_signature_blob(algorithm, raw_signature);
    TEST_ASSERT(auth.verify_signature(session_id, username, algorithm, public_key_blob, signature),
                "ecdsa-sha2-nistp256 publickey fallback should verify a valid signature");
    return true;
}

bool test_build_kex_init_uses_filtered_host_key_algorithms_in_config_order()
{
    SshAlgorithmRegistry registry;
    registry.register_kex("curve25519-sha256", []() { return std::make_unique<FakeKexAlgorithm>(); });
    registry.register_host_key("rsa-sha2-256", []() { return std::make_unique<FakeHostKeyAlgorithm>(); });
    registry.register_host_key("ssh-ed25519", []() { return std::make_unique<FakeHostKeyAlgorithm>(); });
    registry.register_cipher("aes128-ctr", []() { return std::make_unique<FakeCipher>(); });
    registry.register_mac("hmac-sha2-256", []() { return std::make_unique<FakeMac>(); });
    registry.register_compression("none", []() { return std::make_unique<FakeCompression>(); });

    SshCryptoOpenSSL crypto;
    SshTransport transport(&registry, &crypto, true);

    SshServerConfig config;
    config.kex_algorithms = { "curve25519-sha256" };
    config.host_key_algorithms = { "ssh-ed25519", "unsupported-host", "rsa-sha2-256" };
    config.cipher_algorithms = { "aes128-ctr" };
    config.mac_algorithms = { "hmac-sha2-256" };
    config.compression_algorithms = { "none", "zlib" };

    auto encoded = transport.build_kex_init(config);
    auto decoded = SshMessageCodec::decode_kex_init(
        reinterpret_cast<const uint8_t *>(encoded.read_ptr()), encoded.readable_bytes());
    TEST_ASSERT(decoded.has_value(), "kexinit should decode successfully");
    TEST_ASSERT(decoded->server_host_key_algorithms == "ssh-ed25519,rsa-sha2-256",
                "host key algorithms should keep config order and drop unsupported entries");
    TEST_ASSERT(decoded->compression_algorithms_client_to_server == "none",
                "compression list should only advertise supported algorithms");
    return true;
}

bool test_process_kex_init_negotiates_config_preference_and_hash()
{
    SshAlgorithmRegistry registry;
    registry.register_kex("curve25519-sha256", []() { return std::make_unique<FakeKexAlgorithm>(); });
    registry.register_kex("diffie-hellman-group18-sha512", []() { return std::make_unique<FakeKexAlgorithm>(); });
    registry.register_host_key("ssh-ed25519", []() { return std::make_unique<FakeHostKeyAlgorithm>(); });
    registry.register_host_key("rsa-sha2-256", []() { return std::make_unique<FakeHostKeyAlgorithm>(); });
    registry.register_cipher("aes128-ctr", []() { return std::make_unique<FakeCipher>(); });
    registry.register_cipher("aes256-ctr", []() { return std::make_unique<FakeCipher>(); });
    registry.register_mac("hmac-sha2-256", []() { return std::make_unique<FakeMac>(); });
    registry.register_compression("none", []() { return std::make_unique<FakeCompression>(); });

    SshCryptoOpenSSL crypto;
    SshTransport transport(&registry, &crypto, true);

    SshServerConfig config;
    config.kex_algorithms = { "curve25519-sha256", "diffie-hellman-group18-sha512" };
    config.host_key_algorithms = { "ssh-ed25519", "rsa-sha2-256" };
    config.cipher_algorithms = { "aes128-ctr", "aes256-ctr" };
    config.mac_algorithms = { "hmac-sha2-256" };
    config.compression_algorithms = { "none" };

    SshKexInitMessage peer;
    peer.kex_algorithms = "diffie-hellman-group18-sha512,curve25519-sha256";
    peer.server_host_key_algorithms = "rsa-sha2-256,ssh-ed25519";
    peer.encryption_algorithms_client_to_server = "aes256-ctr,aes128-ctr";
    peer.encryption_algorithms_server_to_client = "aes256-ctr,aes128-ctr";
    peer.mac_algorithms_client_to_server = "hmac-sha2-256";
    peer.mac_algorithms_server_to_client = "hmac-sha2-256";
    peer.compression_algorithms_client_to_server = "none";
    peer.compression_algorithms_server_to_client = "none";

    auto negotiated = transport.process_kex_init(peer, config);
    TEST_ASSERT(negotiated.has_value(), "kex negotiation should succeed");
    TEST_ASSERT(negotiated->kex_name == "curve25519-sha256",
                "kex negotiation should prefer server config order over peer order");
    TEST_ASSERT(negotiated->host_key_name == "ssh-ed25519",
                "host key negotiation should prefer server config order");
    TEST_ASSERT(negotiated->client_to_server_cipher_name == "aes128-ctr",
                "cipher negotiation should prefer server config order");
    TEST_ASSERT(negotiated->kex_hash_name == "sha256",
                "curve25519 negotiation should select sha256 hash");

    config.kex_algorithms = { "diffie-hellman-group18-sha512" };
    auto negotiated_sha512 = transport.process_kex_init(peer, config);
    TEST_ASSERT(negotiated_sha512.has_value(), "sha512 kex negotiation should succeed");
    TEST_ASSERT(negotiated_sha512->kex_hash_name == "sha512",
                "group18 negotiation should select sha512 hash");
    return true;
}

bool test_first_kex_packet_follows_ignores_only_wrong_guess()
{
    SshAlgorithmRegistry registry;
    registry.register_kex("curve25519-sha256", []() { return std::make_unique<FakeKexAlgorithm>(); });
    registry.register_kex("diffie-hellman-group18-sha512", []() { return std::make_unique<FakeKexAlgorithm>(); });
    registry.register_host_key("ssh-ed25519", []() { return std::make_unique<FakeHostKeyAlgorithm>(); });
    registry.register_host_key("rsa-sha2-256", []() { return std::make_unique<FakeHostKeyAlgorithm>(); });
    registry.register_cipher("aes128-ctr", []() { return std::make_unique<FakeCipher>(); });
    registry.register_mac("hmac-sha2-256", []() { return std::make_unique<FakeMac>(); });
    registry.register_compression("none", []() { return std::make_unique<FakeCompression>(); });

    SshCryptoOpenSSL crypto;
    SshTransport transport(&registry, &crypto, true);

    SshServerConfig config;
    config.kex_algorithms = { "curve25519-sha256", "diffie-hellman-group18-sha512" };
    config.host_key_algorithms = { "ssh-ed25519", "rsa-sha2-256" };
    config.cipher_algorithms = { "aes128-ctr" };
    config.mac_algorithms = { "hmac-sha2-256" };
    config.compression_algorithms = { "none" };

    SshKexInitMessage wrong_guess = {};
    wrong_guess.kex_algorithms = "diffie-hellman-group18-sha512,curve25519-sha256";
    wrong_guess.server_host_key_algorithms = "rsa-sha2-256,ssh-ed25519";
    wrong_guess.encryption_algorithms_client_to_server = "aes128-ctr";
    wrong_guess.encryption_algorithms_server_to_client = "aes128-ctr";
    wrong_guess.mac_algorithms_client_to_server = "hmac-sha2-256";
    wrong_guess.mac_algorithms_server_to_client = "hmac-sha2-256";
    wrong_guess.compression_algorithms_client_to_server = "none";
    wrong_guess.compression_algorithms_server_to_client = "none";
    wrong_guess.first_kex_packet_follows = true;

    auto wrong_negotiated = transport.process_kex_init(wrong_guess, config);
    TEST_ASSERT(wrong_negotiated.has_value(), "wrong-guess negotiation should still succeed");
    TEST_ASSERT(transport.consume_pending_kex_guess(),
                "mismatched first_kex_packet_follows guess should cause one KEX packet to be ignored");
    TEST_ASSERT(!transport.consume_pending_kex_guess(),
                "ignore flag should only apply to the next KEX packet");

    SshKexInitMessage correct_guess = wrong_guess;
    correct_guess.kex_algorithms = "curve25519-sha256,diffie-hellman-group18-sha512";
    correct_guess.server_host_key_algorithms = "ssh-ed25519,rsa-sha2-256";

    auto correct_negotiated = transport.process_kex_init(correct_guess, config);
    TEST_ASSERT(correct_negotiated.has_value(), "correct-guess negotiation should succeed");
    TEST_ASSERT(!transport.consume_pending_kex_guess(),
                "matching first_kex_packet_follows guess should not skip the next KEX packet");
    return true;
}

bool test_process_newkeys_activates_encryption_after_kex()
{
    SshAlgorithmRegistry registry;
    registry.register_kex("test-kex", []() { return std::make_unique<FakeKexAlgorithm>(); });
    registry.register_host_key("test-host", []() { return std::make_unique<FakeHostKeyAlgorithm>(); });
    registry.register_cipher("test-cipher", []() { return std::make_unique<FakeCipher>(); });
    registry.register_mac("test-mac", []() { return std::make_unique<FakeMac>(); });
    registry.register_compression("none", []() { return std::make_unique<FakeCompression>(); });

    SshCryptoOpenSSL crypto;
    SshTransport transport(&registry, &crypto, true);
    transport.set_host_key_algorithm(std::make_unique<FakeHostKeyAlgorithm>());
    transport.set_client_version("SSH-2.0-Client");
    transport.set_server_version("SSH-2.0-Server");

    SshServerConfig config;
    config.kex_algorithms = { "test-kex" };
    config.host_key_algorithms = { "test-host" };
    config.cipher_algorithms = { "test-cipher" };
    config.mac_algorithms = { "test-mac" };
    config.compression_algorithms = { "none" };

    auto our_kex = transport.build_kex_init(config);
    (void)our_kex;

    SshKexInitMessage peer;
    peer.kex_algorithms = "test-kex";
    peer.server_host_key_algorithms = "test-host";
    peer.encryption_algorithms_client_to_server = "test-cipher";
    peer.encryption_algorithms_server_to_client = "test-cipher";
    peer.mac_algorithms_client_to_server = "test-mac";
    peer.mac_algorithms_server_to_client = "test-mac";
    peer.compression_algorithms_client_to_server = "none";
    peer.compression_algorithms_server_to_client = "none";

    yuan::buffer::ByteBuffer peer_raw = SshMessageCodec::encode_kex_init(peer);
    transport.set_peer_kex_init_raw(std::vector<uint8_t>(
        reinterpret_cast<const uint8_t *>(peer_raw.read_ptr()),
        reinterpret_cast<const uint8_t *>(peer_raw.read_ptr()) + peer_raw.readable_bytes()));

    auto negotiated = transport.process_kex_init(peer, config);
    TEST_ASSERT(negotiated.has_value(), "test kex negotiation should succeed");

    auto reply = transport.process_kex_init_message({ 0x01, 0x02, 0x03 }, "SSH-2.0-Client", "SSH-2.0-Server");
    TEST_ASSERT(reply.has_value(), "kex reply should be generated");
    TEST_ASSERT(!transport.session_id().empty(), "session id should be established from exchange hash");
    TEST_ASSERT(transport.process_newkeys(), "NEWKEYS should activate cipher context");
    TEST_ASSERT(transport.is_encrypted(), "transport should become encrypted after NEWKEYS");
    TEST_ASSERT(transport.state() == SshTransportState::newkeys,
                "transport state should move to newkeys after activation");
    TEST_ASSERT(transport.cipher_context().server_cipher() != nullptr,
                "cipher context should instantiate server cipher");
    TEST_ASSERT(transport.cipher_context().server_mac() != nullptr,
                "cipher context should instantiate server mac for non-AEAD cipher");
    return true;
}

bool test_process_kex_init_message_uses_client_then_server_kex_payloads()
{
    SshAlgorithmRegistry registry;
    registry.register_kex("test-kex", []() { return std::make_unique<FakeKexAlgorithm>(); });
    registry.register_host_key("test-host", []() { return std::make_unique<FakeHostKeyAlgorithm>(); });
    registry.register_cipher("test-cipher", []() { return std::make_unique<FakeCipher>(); });
    registry.register_mac("test-mac", []() { return std::make_unique<FakeMac>(); });
    registry.register_compression("none", []() { return std::make_unique<FakeCompression>(); });

    SshCryptoOpenSSL crypto;
    SshTransport transport(&registry, &crypto, true);
    transport.set_host_key_algorithm(std::make_unique<FakeHostKeyAlgorithm>());
    transport.set_client_version("SSH-2.0-C");
    transport.set_server_version("SSH-2.0-Server-Longer");

    SshServerConfig config;
    config.kex_algorithms = { "test-kex" };
    config.host_key_algorithms = { "test-host" };
    config.cipher_algorithms = { "test-cipher" };
    config.mac_algorithms = { "test-mac" };
    config.compression_algorithms = { "none" };

    transport.build_kex_init(config);

    SshKexInitMessage peer = {};
    peer.kex_algorithms = "test-kex";
    peer.server_host_key_algorithms = "test-host";
    peer.encryption_algorithms_client_to_server = "test-cipher";
    peer.encryption_algorithms_server_to_client = "test-cipher";
    peer.mac_algorithms_client_to_server = "test-mac";
    peer.mac_algorithms_server_to_client = "test-mac";
    peer.compression_algorithms_client_to_server = "none";
    peer.compression_algorithms_server_to_client = "none";
    peer.languages_client_to_server = "zh-CN";

    yuan::buffer::ByteBuffer peer_raw = SshMessageCodec::encode_kex_init(peer);
    transport.set_peer_kex_init_raw(std::vector<uint8_t>(
        reinterpret_cast<const uint8_t *>(peer_raw.read_ptr()),
        reinterpret_cast<const uint8_t *>(peer_raw.read_ptr()) + peer_raw.readable_bytes()));

    auto negotiated = transport.process_kex_init(peer, config);
    TEST_ASSERT(negotiated.has_value(), "negotiation should succeed before computing exchange hash");

    auto reply = transport.process_kex_init_message({ 0x01, 0x02, 0x03 }, "SSH-2.0-C", "SSH-2.0-Server-Longer");
    TEST_ASSERT(reply.has_value(), "reply should be generated");
    TEST_ASSERT(reply->signature.size() >= 5, "fake host key signature should include exchange hash bytes");
    TEST_ASSERT(reply->signature[3] == static_cast<uint8_t>(peer_raw.readable_bytes()),
                "exchange hash should encode peer KEXINIT as client payload");
    TEST_ASSERT(reply->signature[4] == static_cast<uint8_t>(transport.our_kex_init_raw().size()),
                "exchange hash should encode local KEXINIT as server payload");
    return true;
}

bool test_reset_for_rekey_keeps_existing_encryption_active()
{
    SshAlgorithmRegistry registry;
    registry.register_kex("test-kex", []() { return std::make_unique<FakeKexAlgorithm>(); });
    registry.register_host_key("test-host", []() { return std::make_unique<FakeHostKeyAlgorithm>(); });
    registry.register_cipher("test-cipher", []() { return std::make_unique<FakeCipher>(); });
    registry.register_mac("test-mac", []() { return std::make_unique<FakeMac>(); });
    registry.register_compression("none", []() { return std::make_unique<FakeCompression>(); });

    SshCryptoOpenSSL crypto;
    SshTransport transport(&registry, &crypto, true);
    transport.set_host_key_algorithm(std::make_unique<FakeHostKeyAlgorithm>());
    transport.set_client_version("SSH-2.0-Client");
    transport.set_server_version("SSH-2.0-Server");

    SshServerConfig config;
    config.kex_algorithms = { "test-kex" };
    config.host_key_algorithms = { "test-host" };
    config.cipher_algorithms = { "test-cipher" };
    config.mac_algorithms = { "test-mac" };
    config.compression_algorithms = { "none" };

    auto our_kex = transport.build_kex_init(config);
    (void)our_kex;

    SshKexInitMessage peer;
    peer.kex_algorithms = "test-kex";
    peer.server_host_key_algorithms = "test-host";
    peer.encryption_algorithms_client_to_server = "test-cipher";
    peer.encryption_algorithms_server_to_client = "test-cipher";
    peer.mac_algorithms_client_to_server = "test-mac";
    peer.mac_algorithms_server_to_client = "test-mac";
    peer.compression_algorithms_client_to_server = "none";
    peer.compression_algorithms_server_to_client = "none";

    yuan::buffer::ByteBuffer peer_raw = SshMessageCodec::encode_kex_init(peer);
    transport.set_peer_kex_init_raw(std::vector<uint8_t>(
        reinterpret_cast<const uint8_t *>(peer_raw.read_ptr()),
        reinterpret_cast<const uint8_t *>(peer_raw.read_ptr()) + peer_raw.readable_bytes()));

    auto negotiated = transport.process_kex_init(peer, config);
    TEST_ASSERT(negotiated.has_value(), "initial negotiation should succeed");
    auto reply = transport.process_kex_init_message({ 0x01, 0x02, 0x03 }, "SSH-2.0-Client", "SSH-2.0-Server");
    TEST_ASSERT(reply.has_value(), "initial kex reply should succeed");
    TEST_ASSERT(transport.process_newkeys(), "initial NEWKEYS should activate encryption");
    TEST_ASSERT(transport.is_encrypted(), "transport should be encrypted before rekey reset");

    const std::vector<uint8_t> payload = { 0x50, 0x60, 0x70 };
    auto packet_before = transport.encode_packet(payload.data(), payload.size());
    auto decoded_before = transport.decode_packet(
        reinterpret_cast<const uint8_t *>(packet_before.read_ptr()), packet_before.readable_bytes());
    TEST_ASSERT(decoded_before.has_value() && *decoded_before == payload,
                "encrypted transport should roundtrip payload before rekey reset");

    transport.reset_for_rekey();
    TEST_ASSERT(transport.state() == SshTransportState::kex_init,
                "reset_for_rekey should move transport back to kex_init");
    TEST_ASSERT(transport.is_encrypted(),
                "reset_for_rekey should keep the previously active cipher context alive");
    TEST_ASSERT(transport.our_kex_init_raw().empty(),
                "reset_for_rekey should clear previous local KEXINIT payload");

    auto packet_after = transport.encode_packet(payload.data(), payload.size());
    auto decoded_after = transport.decode_packet(
        reinterpret_cast<const uint8_t *>(packet_after.read_ptr()), packet_after.readable_bytes());
    TEST_ASSERT(decoded_after.has_value() && *decoded_after == payload,
                "old keys should still protect traffic until the next NEWKEYS arrives");
    return true;
}

bool test_rekey_preserves_session_id_across_new_exchange()
{
    SshAlgorithmRegistry registry;
    registry.register_kex("test-kex", []() { return std::make_unique<FakeKexAlgorithm>(); });
    registry.register_host_key("test-host", []() { return std::make_unique<FakeHostKeyAlgorithm>(); });
    registry.register_cipher("test-cipher", []() { return std::make_unique<FakeCipher>(); });
    registry.register_mac("test-mac", []() { return std::make_unique<FakeMac>(); });
    registry.register_compression("none", []() { return std::make_unique<FakeCompression>(); });

    SshCryptoOpenSSL crypto;
    SshTransport transport(&registry, &crypto, true);
    transport.set_host_key_algorithm(std::make_unique<FakeHostKeyAlgorithm>());
    transport.set_client_version("SSH-2.0-Client");
    transport.set_server_version("SSH-2.0-Server");

    SshServerConfig config;
    config.kex_algorithms = { "test-kex" };
    config.host_key_algorithms = { "test-host" };
    config.cipher_algorithms = { "test-cipher" };
    config.mac_algorithms = { "test-mac" };
    config.compression_algorithms = { "none" };

    auto build_peer_raw = [](const SshKexInitMessage & peer)
    {
        yuan::buffer::ByteBuffer raw = SshMessageCodec::encode_kex_init(peer);
        return std::vector<uint8_t>(
            reinterpret_cast<const uint8_t *>(raw.read_ptr()),
            reinterpret_cast<const uint8_t *>(raw.read_ptr()) + raw.readable_bytes());
    };

    SshKexInitMessage peer;
    peer.kex_algorithms = "test-kex";
    peer.server_host_key_algorithms = "test-host";
    peer.encryption_algorithms_client_to_server = "test-cipher";
    peer.encryption_algorithms_server_to_client = "test-cipher";
    peer.mac_algorithms_client_to_server = "test-mac";
    peer.mac_algorithms_server_to_client = "test-mac";
    peer.compression_algorithms_client_to_server = "none";
    peer.compression_algorithms_server_to_client = "none";

    transport.build_kex_init(config);
    transport.set_peer_kex_init_raw(build_peer_raw(peer));
    auto initial_negotiated = transport.process_kex_init(peer, config);
    TEST_ASSERT(initial_negotiated.has_value(), "initial negotiation should succeed");
    auto initial_reply = transport.process_kex_init_message({ 0x01, 0x02, 0x03 }, "SSH-2.0-Client", "SSH-2.0-Server");
    TEST_ASSERT(initial_reply.has_value(), "initial reply should succeed");
    TEST_ASSERT(transport.process_newkeys(), "initial NEWKEYS should succeed");
    const auto initial_session_id = transport.session_id();
    TEST_ASSERT(!initial_session_id.empty(), "initial KEX should establish a session id");

    transport.reset_for_rekey();
    transport.build_kex_init(config);
    transport.set_peer_kex_init_raw(build_peer_raw(peer));
    auto rekey_negotiated = transport.process_kex_init(peer, config);
    TEST_ASSERT(rekey_negotiated.has_value(), "rekey negotiation should succeed");
    auto rekey_reply = transport.process_kex_init_message({ 0x09, 0x08, 0x07 }, "SSH-2.0-Client", "SSH-2.0-Server");
    TEST_ASSERT(rekey_reply.has_value(), "rekey reply should succeed");
    TEST_ASSERT(transport.process_newkeys(), "rekey NEWKEYS should succeed");
    TEST_ASSERT(transport.session_id() == initial_session_id,
                "rekey must preserve the original session id");
    TEST_ASSERT(transport.is_encrypted(), "transport should remain encrypted after rekey");
    return true;
}

bool test_packet_codec_plaintext_roundtrip_and_partial_parse()
{
    const std::vector<uint8_t> payload = { 0x32, 0x00, 0x00, 0x00, 0x01 };
    auto packet = SshPacketCodec::encode(0, payload.data(), payload.size(), nullptr);

    yuan::buffer::ByteBuffer partial;
    partial.append(packet.read_ptr(), 3);
    auto partial_parse = SshPacketCodec::try_parse(partial, false, nullptr, 0);
    TEST_ASSERT(!partial_parse.complete, "plaintext parse should wait for full length field");

    auto full_parse = SshPacketCodec::try_parse(packet, false, nullptr, 0);
    TEST_ASSERT(full_parse.complete, "plaintext packet should parse completely");
    TEST_ASSERT(full_parse.total_bytes == packet.readable_bytes(),
                "plaintext parse length should match encoded packet size");

    auto decoded = SshPacketCodec::decode(
        0, reinterpret_cast<const uint8_t *>(packet.read_ptr()), packet.readable_bytes(), nullptr);
    TEST_ASSERT(decoded.has_value(), "plaintext packet should decode");
    TEST_ASSERT(*decoded == payload, "plaintext decode should recover original payload");
    return true;
}

bool test_local_file_system_basic_roundtrip()
{
#if YUAN_ENABLE_SSH_SFTP
    const auto root = make_temp_ssh_dir();
    std::error_code cleanup_ec;

    {
        SshLocalFileSystem fs(root.string());

        auto mkdir_result = fs.mkdir("/subdir", {});
        TEST_ASSERT(mkdir_result.success, "local fs should create directories");

        const uint32_t open_flags =
            static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_READ) |
            static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_WRITE) |
            static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_CREAT) |
            static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_TRUNC);
        auto open_result = fs.open("/subdir/note.txt", open_flags, {});
        TEST_ASSERT(open_result.success, "local fs should open files for read/write");

        const std::vector<uint8_t> payload = { 'h', 'e', 'l', 'l', 'o' };
        auto write_result = fs.write(open_result.handle, 0, payload.data(), static_cast<uint32_t>(payload.size()));
        TEST_ASSERT(write_result.success, "local fs should write file contents");

        auto stat_result = fs.stat("/subdir/note.txt");
        TEST_ASSERT(stat_result.success, "local fs should stat created file");
        TEST_ASSERT(stat_result.attrs.size == payload.size(), "stat should reflect file size");

        auto read_result = fs.read(open_result.handle, 0, 32);
        TEST_ASSERT(read_result.success, "local fs should read written file contents");
        TEST_ASSERT(read_result.data == payload, "read should return the bytes that were written");

        auto close_result = fs.close(open_result.handle);
        TEST_ASSERT(close_result.success, "local fs should close file handle");

        auto realpath_result = fs.realpath("/subdir/note.txt");
        TEST_ASSERT(realpath_result.success, "local fs should resolve realpath inside root");
        TEST_ASSERT(realpath_result.path == "/subdir/note.txt", "realpath should map back to logical SFTP path");

        auto dir_result = fs.opendir("/subdir");
        TEST_ASSERT(dir_result.success, "local fs should open directories");

        auto readdir_result = fs.readdir(dir_result.handle);
        TEST_ASSERT(readdir_result.success, "local fs should enumerate directory entries");
        TEST_ASSERT(!readdir_result.entries.empty(), "readdir should return created file");
        bool saw_note = false;
        for (const auto & entry : readdir_result.entries) {
            if (entry.filename == "note.txt") {
                saw_note = true;
                break;
            }
        }
        TEST_ASSERT(saw_note, "readdir should include the created file");

        auto close_dir_result = fs.close(dir_result.handle);
        TEST_ASSERT(close_dir_result.success, "local fs should close directory handle");
    }

    std::filesystem::remove_all(root, cleanup_ec);
#endif
    return true;
}

bool test_local_file_system_rejects_uid_gid_setstat()
{
#if YUAN_ENABLE_SSH_SFTP
    const auto root = make_temp_ssh_dir();
    std::error_code cleanup_ec;

    {
        SshLocalFileSystem fs(root.string());
        const uint32_t open_flags =
            static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_READ) |
            static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_WRITE) |
            static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_CREAT) |
            static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_TRUNC);
        auto open_result = fs.open("/owner.txt", open_flags, {});
        TEST_ASSERT(open_result.success, "local fs should create file for uid/gid test");

        SftpFileAttrs attrs;
        attrs.flags = SSH_FILEXFER_ATTR_UIDGID;
        attrs.uid = 1000;
        attrs.gid = 1000;

        auto setstat_result = fs.setstat("/owner.txt", attrs);
        TEST_ASSERT(setstat_result.status == SftpStatus::SSH_FX_OP_UNSUPPORTED,
                    "uid/gid setstat should fail explicitly when unsupported");

        auto fsetstat_result = fs.fsetstat(open_result.handle, attrs);
        TEST_ASSERT(fsetstat_result.status == SftpStatus::SSH_FX_OP_UNSUPPORTED,
                    "uid/gid fsetstat should fail explicitly when unsupported");

        auto close_result = fs.close(open_result.handle);
        TEST_ASSERT(close_result.success, "local fs should close file after uid/gid test");
    }

    std::filesystem::remove_all(root, cleanup_ec);
#endif
    return true;
}

bool test_local_file_system_absolute_symlink_targets_stay_logical()
{
#if YUAN_ENABLE_SSH_SFTP
    const auto root = make_temp_ssh_dir();
    std::error_code cleanup_ec;

    {
        SshLocalFileSystem fs(root.string());
        auto mkdir_result = fs.mkdir("/subdir", {});
        TEST_ASSERT(mkdir_result.success, "local fs should create subdir for symlink test");

        const uint32_t open_flags =
            static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_READ) |
            static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_WRITE) |
            static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_CREAT) |
            static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_TRUNC);
        auto open_result = fs.open("/subdir/target.txt", open_flags, {});
        TEST_ASSERT(open_result.success, "local fs should create symlink target file");
        auto close_result = fs.close(open_result.handle);
        TEST_ASSERT(close_result.success, "local fs should close symlink target file");

        auto invalid_target_result = fs.symlink("/escape.txt", "/../../outside.txt");
        TEST_ASSERT(invalid_target_result.status == SftpStatus::SSH_FX_NO_SUCH_PATH,
                    "absolute symlink target outside root should be rejected");

        auto symlink_result = fs.symlink("/link.txt", "/subdir/target.txt");
        if (symlink_result.success) {
            auto readlink_result = fs.readlink("/link.txt");
            TEST_ASSERT(readlink_result.success, "readlink should succeed after creating a symlink");
            TEST_ASSERT(readlink_result.link_target == "/subdir/target.txt",
                        "readlink should expose the logical SFTP path instead of host filesystem path");
        } else {
            TEST_ASSERT(symlink_result.status == SftpStatus::SSH_FX_PERMISSION_DENIED ||
                            symlink_result.status == SftpStatus::SSH_FX_OP_UNSUPPORTED,
                        "symlink failure should be explicit when the platform or permissions do not allow it");
        }
    }

    std::filesystem::remove_all(root, cleanup_ec);
#endif
    return true;
}

bool test_local_file_system_realpath_and_readlink_edge_inputs()
{
#if YUAN_ENABLE_SSH_SFTP
    const auto root = make_temp_ssh_dir();
    std::error_code cleanup_ec;

    {
        SshLocalFileSystem fs(root.string());
        auto mkdir_result = fs.mkdir("/subdir", {});
        TEST_ASSERT(mkdir_result.success, "local fs should create subdir for edge input test");

        const uint32_t open_flags =
            static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_READ) |
            static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_WRITE) |
            static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_CREAT) |
            static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_TRUNC);
        auto open_result = fs.open("/subdir/target.txt", open_flags, {});
        TEST_ASSERT(open_result.success, "local fs should create target file for edge input test");
        auto close_result = fs.close(open_result.handle);
        TEST_ASSERT(close_result.success, "local fs should close target file for edge input test");

        auto realpath_root = fs.realpath("/");
        TEST_ASSERT(realpath_root.success, "realpath should resolve root path");
        TEST_ASSERT(realpath_root.path == "/", "realpath root should stay logical root");

        auto realpath_mixed = fs.realpath("//subdir///target.txt");
        TEST_ASSERT(realpath_mixed.success, "realpath should normalize repeated separators");
        TEST_ASSERT(realpath_mixed.path == "/subdir/target.txt",
                    "realpath should normalize repeated separators into canonical logical path");

        auto realpath_parent = fs.realpath("/subdir/../target.txt");
        TEST_ASSERT(realpath_parent.status == SftpStatus::SSH_FX_NO_SUCH_PATH,
                    "realpath should reject parent traversal segments in client input");

        auto symlink_result = fs.symlink("/rel_link.txt", "subdir/target.txt");
        if (symlink_result.success) {
            auto readlink_result = fs.readlink("/rel_link.txt");
            TEST_ASSERT(readlink_result.success, "readlink should succeed for relative symlink target");
            TEST_ASSERT(readlink_result.link_target == "/subdir/target.txt",
                        "readlink should expose normalized logical path for in-root relative target");
        } else {
            TEST_ASSERT(symlink_result.status == SftpStatus::SSH_FX_PERMISSION_DENIED ||
                            symlink_result.status == SftpStatus::SSH_FX_OP_UNSUPPORTED,
                        "symlink failure should be explicit when platform or permissions do not allow it");
        }
    }

    std::filesystem::remove_all(root, cleanup_ec);
#endif
    return true;
}

bool test_try_parse_marks_invalid_for_encrypted_bytes_before_newkeys()
{
    SshAlgorithmRegistry registry;
    registry.register_cipher("test-opaque", []() { return std::make_unique<FakeOpaqueCipher>(); });
    registry.register_mac("test-mac", []() { return std::make_unique<FakeMac>(); });
    registry.register_compression("none", []() { return std::make_unique<FakeCompression>(); });

    auto active_ctx = build_active_cipher_context(registry, "test-opaque", true);
    const std::vector<uint8_t> payload = { 0x21, 0x43, 0x65, 0x87 };
    auto encrypted_packet = SshPacketCodec::encode(11, payload.data(), payload.size(), &active_ctx);

    auto parse = SshPacketCodec::try_parse(encrypted_packet, false, nullptr, 0);
    TEST_ASSERT(!parse.complete, "pre-NEWKEYS encrypted bytes should not parse as a complete plaintext packet");
    TEST_ASSERT(parse.invalid, "pre-NEWKEYS encrypted bytes should be marked invalid rather than incomplete");
    return true;
}

bool test_packet_codec_encrypted_roundtrip_and_mac_failure()
{
    SshAlgorithmRegistry registry;
    registry.register_cipher("test-cipher", []() { return std::make_unique<FakeCipher>(); });
    registry.register_mac("test-mac", []() { return std::make_unique<FakeMac>(); });
    registry.register_compression("none", []() { return std::make_unique<FakeCompression>(); });
    auto ctx = build_active_cipher_context(registry, "test-cipher", true);

    const std::vector<uint8_t> payload = { 0x15, 0xAA, 0xBB, 0xCC };
    auto packet = SshPacketCodec::encode(7, payload.data(), payload.size(), &ctx);

    auto parse = SshPacketCodec::try_parse(packet, true, &ctx, 7);
    TEST_ASSERT(parse.complete, "encrypted packet with MAC should parse");

    auto decoded = SshPacketCodec::decode(
        7, reinterpret_cast<const uint8_t *>(packet.read_ptr()), packet.readable_bytes(), &ctx);
    TEST_ASSERT(decoded.has_value(), "encrypted packet with MAC should decode");
    TEST_ASSERT(*decoded == payload, "encrypted decode should recover original payload");

    auto tampered = packet;
    tampered.data()[tampered.write_offset() - 1] ^= 0x01;
    auto tampered_decoded = SshPacketCodec::decode(
        7, reinterpret_cast<const uint8_t *>(tampered.read_ptr()), tampered.readable_bytes(), &ctx);
    TEST_ASSERT(!tampered_decoded.has_value(), "tampered MAC should fail decode");
    return true;
}

bool test_packet_codec_aead_roundtrip_and_tag_failure()
{
    SshAlgorithmRegistry registry;
    registry.register_cipher("test-aead", []() { return std::make_unique<FakeAeadCipher>(); });
    registry.register_compression("none", []() { return std::make_unique<FakeCompression>(); });
    auto ctx = build_active_cipher_context(registry, "test-aead", false);

    const std::vector<uint8_t> payload = { 0x5A, 0x10, 0x20, 0x30 };
    auto packet = SshPacketCodec::encode(3, payload.data(), payload.size(), &ctx);

    auto parse = SshPacketCodec::try_parse(packet, true, &ctx, 3);
    TEST_ASSERT(parse.complete, "AEAD packet should parse");

    const auto * packet_ptr = reinterpret_cast<const uint8_t *>(packet.read_ptr());
    const uint32_t packet_length = (static_cast<uint32_t>(packet_ptr[0]) << 24) |
                                   (static_cast<uint32_t>(packet_ptr[1]) << 16) |
                                   (static_cast<uint32_t>(packet_ptr[2]) << 8) |
                                   static_cast<uint32_t>(packet_ptr[3]);
    uint8_t expected_checksum = 0;
    for (size_t i = 0; i < 4; ++i) {
        expected_checksum ^= packet_ptr[i];
    }
    for (size_t i = 0; i < packet_length; ++i) {
        expected_checksum ^= packet_ptr[4 + i];
    }
    TEST_ASSERT(packet_ptr[4 + packet_length] == expected_checksum,
                "AEAD tag should be derived from packet length and ciphertext");

    std::vector<uint8_t> decrypted_packet;
    TEST_ASSERT(ctx.decrypt_packet(
                    3,
                    reinterpret_cast<const uint8_t *>(packet.read_ptr()) + 4,
                    packet.readable_bytes() - 4,
                    decrypted_packet),
                "AEAD cipher context should decrypt encoded packet body");

    auto decoded = SshPacketCodec::decode(
        3, reinterpret_cast<const uint8_t *>(packet.read_ptr()), packet.readable_bytes(), &ctx);
    TEST_ASSERT(decoded.has_value(), "AEAD packet should decode");
    TEST_ASSERT(*decoded == payload, "AEAD decode should recover original payload");

    auto tampered = packet;
    tampered.data()[tampered.write_offset() - 1] ^= 0x01;
    auto tampered_decoded = SshPacketCodec::decode(
        3, reinterpret_cast<const uint8_t *>(tampered.read_ptr()), tampered.readable_bytes(), &ctx);
    TEST_ASSERT(!tampered_decoded.has_value(), "tampered AEAD tag should fail decode");
    return true;
}

bool test_channel_request_rejects_second_exec_and_second_shell()
{
    SshConnectionManager mgr(nullptr);
    auto * channel = mgr.create_channel(SSH_CHANNEL_SESSION, 21, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");

    SshChannelRequestMessage exec_msg;
    exec_msg.recipient_channel = 21;
    exec_msg.request_type = "exec";
    exec_msg.want_reply = true;
    exec_msg.request_specific_data = encode_string_payload("echo first");

    auto first = mgr.handle_channel_request(exec_msg, nullptr);
    auto first_span = first.readable_span();
    TEST_ASSERT(!first_span.empty(), "first exec should respond");
    TEST_ASSERT(first_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_FAILURE),
                "default handler should reject exec");

    auto second = mgr.handle_channel_request(exec_msg, nullptr);
    auto second_span = second.readable_span();
    TEST_ASSERT(!second_span.empty(), "second exec should respond");
    TEST_ASSERT(second_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_FAILURE),
                "second exec request must be rejected");

    SshChannelRequestMessage shell_msg;
    shell_msg.recipient_channel = 21;
    shell_msg.request_type = "shell";
    shell_msg.want_reply = true;

    auto shell_reply = mgr.handle_channel_request(shell_msg, nullptr);
    auto shell_span = shell_reply.readable_span();
    TEST_ASSERT(!shell_span.empty(), "shell should respond");
    TEST_ASSERT(shell_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_FAILURE),
                "shell must be rejected after a previous command request");
    return true;
}

bool test_channel_request_rejects_second_pty_and_parses_modes_as_string()
{
    class RecordingPtyHandler final : public SshHandler
    {
    public:
        bool on_channel_open(SshSession * session,
                             const std::string & channel_type,
                             SshChannel * channel) override
        {
            (void)session;
            (void)channel;
            return channel_type == SSH_CHANNEL_SESSION;
        }

        bool on_pty_request(SshSession * session,
                            SshChannel * channel,
                            const std::string & term,
                            uint32_t width, uint32_t height,
                            uint32_t pixel_width, uint32_t pixel_height,
                            const std::vector<uint8_t> & modes) override
        {
            (void)session;
            (void)channel;
            last_term = term;
            last_width = width;
            last_height = height;
            last_pixel_width = pixel_width;
            last_pixel_height = pixel_height;
            last_modes = modes;
            calls++;
            return true;
        }

        int calls = 0;
        std::string last_term;
        uint32_t last_width = 0;
        uint32_t last_height = 0;
        uint32_t last_pixel_width = 0;
        uint32_t last_pixel_height = 0;
        std::vector<uint8_t> last_modes;
    };

    SshConnectionManager mgr(nullptr);
    auto * channel = mgr.create_channel(SSH_CHANNEL_SESSION, 22, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");

    yuan::buffer::ByteBuffer pty_payload;
    SshMessageCodec::write_string(pty_payload, "xterm-256color");
    SshMessageCodec::write_uint32(pty_payload, 120);
    SshMessageCodec::write_uint32(pty_payload, 40);
    SshMessageCodec::write_uint32(pty_payload, 1000);
    SshMessageCodec::write_uint32(pty_payload, 800);
    SshMessageCodec::write_string(pty_payload, std::string("\x01\x00\x00\x00\x00\x00", 6));

    auto pty_span = pty_payload.readable_span();
    SshChannelRequestMessage pty_msg;
    pty_msg.recipient_channel = 22;
    pty_msg.request_type = "pty-req";
    pty_msg.want_reply = true;
    pty_msg.request_specific_data.assign(
        reinterpret_cast<const uint8_t *>(pty_span.data()),
        reinterpret_cast<const uint8_t *>(pty_span.data()) + pty_span.size());

    RecordingPtyHandler handler;
    auto first = mgr.handle_channel_request(pty_msg, &handler);
    auto first_span = first.readable_span();
    TEST_ASSERT(!first_span.empty(), "first pty should respond");
    TEST_ASSERT(first_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_SUCCESS),
                "first pty should be accepted");
    TEST_ASSERT(handler.calls == 1, "pty callback should be called once");
    TEST_ASSERT(handler.last_term == "xterm-256color", "pty term should decode correctly");
    TEST_ASSERT(handler.last_width == 120 && handler.last_height == 40,
                "pty dimensions should decode correctly");
    TEST_ASSERT(handler.last_pixel_width == 1000 && handler.last_pixel_height == 800,
                "pty pixel dimensions should decode correctly");
    TEST_ASSERT(handler.last_modes.size() == 6,
                "terminal modes should preserve exact length from SSH string payload");

    auto second = mgr.handle_channel_request(pty_msg, &handler);
    auto second_span = second.readable_span();
    TEST_ASSERT(!second_span.empty(), "second pty should respond");
    TEST_ASSERT(second_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_FAILURE),
                "second pty request must be rejected");
    TEST_ASSERT(handler.calls == 1, "rejected second pty should not invoke callback again");
    return true;
}

bool test_window_change_and_signal_always_reply_success_when_want_reply()
{
    SshConnectionManager mgr(nullptr);
    auto * channel = mgr.create_channel(SSH_CHANNEL_SESSION, 23, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");

    yuan::buffer::ByteBuffer wc_payload;
    SshMessageCodec::write_uint32(wc_payload, 90);
    SshMessageCodec::write_uint32(wc_payload, 30);
    SshMessageCodec::write_uint32(wc_payload, 900);
    SshMessageCodec::write_uint32(wc_payload, 700);

    auto wc_span = wc_payload.readable_span();
    SshChannelRequestMessage wc_msg;
    wc_msg.recipient_channel = 23;
    wc_msg.request_type = "window-change";
    wc_msg.want_reply = true;
    wc_msg.request_specific_data.assign(
        reinterpret_cast<const uint8_t *>(wc_span.data()),
        reinterpret_cast<const uint8_t *>(wc_span.data()) + wc_span.size());

    auto wc_reply = mgr.handle_channel_request(wc_msg, nullptr);
    auto wc_reply_span = wc_reply.readable_span();
    TEST_ASSERT(!wc_reply_span.empty(), "window-change should respond when want_reply=true");
    TEST_ASSERT(wc_reply_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_SUCCESS),
                "window-change should return success when parsed");

    SshChannelRequestMessage sig_msg;
    sig_msg.recipient_channel = 23;
    sig_msg.request_type = "signal";
    sig_msg.want_reply = true;
    sig_msg.request_specific_data = encode_string_payload("TERM");

    auto sig_reply = mgr.handle_channel_request(sig_msg, nullptr);
    auto sig_span = sig_reply.readable_span();
    TEST_ASSERT(!sig_span.empty(), "signal should respond when want_reply=true");
    TEST_ASSERT(sig_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_SUCCESS),
                "signal should return success when parsed");
    return true;
}

bool test_direct_tcpip_open_requires_complete_type_specific_payload()
{
    SshConnectionManager mgr(nullptr);

    SshChannelOpenMessage msg;
    msg.channel_type = SSH_CHANNEL_DIRECT_TCPIP;
    msg.sender_channel = 41;
    msg.initial_window_size = SSH_DEFAULT_WINDOW_SIZE;
    msg.maximum_packet_size = SSH_DEFAULT_MAX_PACKET_SIZE;

    {
        yuan::buffer::ByteBuffer payload;
        SshMessageCodec::write_string(payload, "127.0.0.1");
        SshMessageCodec::write_uint32(payload, 8080);
        auto span = payload.readable_span();
        msg.type_specific_data.assign(
            reinterpret_cast<const uint8_t *>(span.data()),
            reinterpret_cast<const uint8_t *>(span.data()) + span.size());
    }

    auto incomplete_response = mgr.handle_channel_open(msg, &SshHandler::default_handler());
    auto incomplete_span = incomplete_response.readable_span();
    TEST_ASSERT(!incomplete_span.empty(), "incomplete direct-tcpip payload should be rejected with a response");
    auto incomplete_decoded = SshMessageCodec::decode_channel_open_failure(
        reinterpret_cast<const uint8_t *>(incomplete_span.data()), incomplete_span.size());
    TEST_ASSERT(incomplete_decoded.has_value(), "incomplete direct-tcpip payload should return OPEN_FAILURE");

    {
        yuan::buffer::ByteBuffer payload;
        SshMessageCodec::write_string(payload, "127.0.0.1");
        SshMessageCodec::write_uint32(payload, 8080);
        SshMessageCodec::write_string(payload, "10.0.0.8");
        SshMessageCodec::write_uint32(payload, 50022);
        payload.append_u8(0x7F);
        auto span = payload.readable_span();
        msg.type_specific_data.assign(
            reinterpret_cast<const uint8_t *>(span.data()),
            reinterpret_cast<const uint8_t *>(span.data()) + span.size());
    }

    auto trailing_response = mgr.handle_channel_open(msg, &SshHandler::default_handler());
    auto trailing_span = trailing_response.readable_span();
    TEST_ASSERT(!trailing_span.empty(), "trailing bytes in direct-tcpip payload should be rejected");
    auto trailing_decoded = SshMessageCodec::decode_channel_open_failure(
        reinterpret_cast<const uint8_t *>(trailing_span.data()), trailing_span.size());
    TEST_ASSERT(trailing_decoded.has_value(), "trailing bytes should return OPEN_FAILURE");
    return true;
}

bool test_direct_tcpip_open_valid_payload_is_still_handled_by_handler_policy()
{
    SshConnectionManager mgr(nullptr);

    SshChannelOpenMessage msg;
    msg.channel_type = SSH_CHANNEL_DIRECT_TCPIP;
    msg.sender_channel = 42;
    msg.initial_window_size = SSH_DEFAULT_WINDOW_SIZE;
    msg.maximum_packet_size = SSH_DEFAULT_MAX_PACKET_SIZE;

    yuan::buffer::ByteBuffer payload;
    SshMessageCodec::write_string(payload, "127.0.0.1");
    SshMessageCodec::write_uint32(payload, 8080);
    SshMessageCodec::write_string(payload, "10.0.0.8");
    SshMessageCodec::write_uint32(payload, 50022);
    auto span = payload.readable_span();
    msg.type_specific_data.assign(
        reinterpret_cast<const uint8_t *>(span.data()),
        reinterpret_cast<const uint8_t *>(span.data()) + span.size());

    auto response = mgr.handle_channel_open(msg, &SshHandler::default_handler());
    auto resp_span = response.readable_span();
    TEST_ASSERT(!resp_span.empty(), "valid direct-tcpip payload should produce a response");
    auto decoded = SshMessageCodec::decode_channel_open_failure(
        reinterpret_cast<const uint8_t *>(resp_span.data()), resp_span.size());
    TEST_ASSERT(decoded.has_value(), "default handler should still deny direct-tcpip by policy");
    TEST_ASSERT(decoded->reason_code == static_cast<uint32_t>(SshChannelOpenFailureReason::SSH_OPEN_ADMINISTRATIVELY_PROHIBITED),
                "valid direct-tcpip payload should be denied by handler policy, not parse failure");
    return true;
}

bool test_build_channel_exit_status_encodes_standard_channel_request()
{
    SshConnectionManager mgr(nullptr);
    const auto packet = mgr.build_channel_exit_status(7, 123);
    auto span = packet.readable_span();
    TEST_ASSERT(!span.empty(), "exit-status packet should not be empty");

    auto decoded = SshMessageCodec::decode_channel_request(
        reinterpret_cast<const uint8_t *>(span.data()), span.size());
    TEST_ASSERT(decoded.has_value(), "exit-status should decode as channel request");
    TEST_ASSERT(decoded->recipient_channel == 7, "exit-status recipient should match");
    TEST_ASSERT(decoded->request_type == "exit-status", "request type should be exit-status");
    TEST_ASSERT(decoded->want_reply == false, "exit-status should not request a reply");

    size_t offset = 0;
    uint32_t status = SshMessageCodec::read_uint32(decoded->request_specific_data.data(),
                                                   decoded->request_specific_data.size(), offset);
    TEST_ASSERT(offset == decoded->request_specific_data.size(),
                "exit-status payload should only contain a single uint32");
    TEST_ASSERT(status == 123, "exit-status payload should keep status code");
    return true;
}

bool test_build_channel_exit_signal_encodes_standard_channel_request()
{
    SshConnectionManager mgr(nullptr);
    const auto packet = mgr.build_channel_exit_signal(9, "TERM", true, "terminated", "en-US");
    auto span = packet.readable_span();
    TEST_ASSERT(!span.empty(), "exit-signal packet should not be empty");

    auto decoded = SshMessageCodec::decode_channel_request(
        reinterpret_cast<const uint8_t *>(span.data()), span.size());
    TEST_ASSERT(decoded.has_value(), "exit-signal should decode as channel request");
    TEST_ASSERT(decoded->recipient_channel == 9, "exit-signal recipient should match");
    TEST_ASSERT(decoded->request_type == "exit-signal", "request type should be exit-signal");
    TEST_ASSERT(decoded->want_reply == false, "exit-signal should not request a reply");

    size_t offset = 0;
    auto signal_name = SshMessageCodec::read_string(decoded->request_specific_data.data(),
                                                    decoded->request_specific_data.size(), offset);
    TEST_ASSERT(signal_name.has_value() && *signal_name == "TERM", "exit-signal should encode signal name");
    const bool core_dumped = SshMessageCodec::read_boolean(decoded->request_specific_data.data(),
                                                           decoded->request_specific_data.size(), offset);
    TEST_ASSERT(core_dumped, "exit-signal should encode core-dumped flag");
    auto error_message = SshMessageCodec::read_string(decoded->request_specific_data.data(),
                                                      decoded->request_specific_data.size(), offset);
    TEST_ASSERT(error_message.has_value() && *error_message == "terminated", "exit-signal should encode error message");
    auto language_tag = SshMessageCodec::read_string(decoded->request_specific_data.data(),
                                                     decoded->request_specific_data.size(), offset);
    TEST_ASSERT(language_tag.has_value() && *language_tag == "en-US", "exit-signal should encode language tag");
    TEST_ASSERT(offset == decoded->request_specific_data.size(),
                "exit-signal payload should be fully consumed");
    return true;
}

bool test_session_dispatch_channel_close_emits_exit_status_before_close()
{
    SshSession session(1001, nullptr);
    auto &mgr = session.connection_manager();
    auto *channel = mgr.create_channel(SSH_CHANNEL_SESSION, 55, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");
    TEST_ASSERT(channel->mark_command_started(), "channel should enter command-started state");

    SshChannelCloseMessage close_msg;
    close_msg.recipient_channel = 55;
    auto close_payload_buf = SshMessageCodec::encode_channel_close(close_msg);
    auto close_span = close_payload_buf.readable_span();
    std::vector<uint8_t> close_payload(
        reinterpret_cast<const uint8_t *>(close_span.data()),
        reinterpret_cast<const uint8_t *>(close_span.data()) + close_span.size());

    session.set_state(SshSession::State::active);
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_CLOSE, close_payload, nullptr);

    auto outgoing = session.drain_outgoing();
    TEST_ASSERT(outgoing.size() == 2,
                "channel close for command channel should enqueue exit-status then channel-close");

    auto first_span = outgoing[0].readable_span();
    auto second_span = outgoing[1].readable_span();
    TEST_ASSERT(!first_span.empty() && !second_span.empty(), "outgoing packets should be non-empty");

    auto first_req = SshMessageCodec::decode_channel_request(
        reinterpret_cast<const uint8_t *>(first_span.data()), first_span.size());
    TEST_ASSERT(first_req.has_value(), "first response should decode as channel request");
    TEST_ASSERT(first_req->request_type == "exit-status", "first response should be exit-status");

    auto second_close = SshMessageCodec::decode_channel_close(
        reinterpret_cast<const uint8_t *>(second_span.data()), second_span.size());
    TEST_ASSERT(second_close.has_value(), "second response should decode as channel-close");
    TEST_ASSERT(second_close->recipient_channel == 55,
                "channel-close should target original remote recipient channel");
    return true;
}

bool test_session_dispatch_channel_close_without_command_sends_only_close()
{
    SshSession session(1002, nullptr);
    auto &mgr = session.connection_manager();
    auto *channel = mgr.create_channel(SSH_CHANNEL_SESSION, 56, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");

    SshChannelCloseMessage close_msg;
    close_msg.recipient_channel = 56;
    auto close_payload_buf = SshMessageCodec::encode_channel_close(close_msg);
    auto close_span = close_payload_buf.readable_span();
    std::vector<uint8_t> close_payload(
        reinterpret_cast<const uint8_t *>(close_span.data()),
        reinterpret_cast<const uint8_t *>(close_span.data()) + close_span.size());

    session.set_state(SshSession::State::active);
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_CLOSE, close_payload, nullptr);

    auto outgoing = session.drain_outgoing();
    TEST_ASSERT(outgoing.size() == 1,
                "non-command channel close should only enqueue channel-close response");

    auto only_span = outgoing[0].readable_span();
    auto decoded_close = SshMessageCodec::decode_channel_close(
        reinterpret_cast<const uint8_t *>(only_span.data()), only_span.size());
    TEST_ASSERT(decoded_close.has_value(), "response should decode as channel-close");
    TEST_ASSERT(decoded_close->recipient_channel == 56,
                "channel-close should target original remote recipient channel");
    return true;
}

bool test_session_dispatch_channel_close_can_emit_exit_signal_via_handler()
{
    class ExitSignalHandler final : public SshHandler
    {
    public:
        SshCommandExitInfo on_command_exit(SshSession *session,
                                           SshChannel *channel) override
        {
            (void)session;
            (void)channel;
            SshCommandExitInfo info;
            info.use_signal = true;
            info.signal_name = "TERM";
            info.core_dumped = false;
            info.error_message = "terminated by policy";
            info.language_tag = "en-US";
            return info;
        }
    };

    SshSession session(1003, nullptr);
    auto &mgr = session.connection_manager();
    auto *channel = mgr.create_channel(SSH_CHANNEL_SESSION, 57, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");
    TEST_ASSERT(channel->mark_command_started(), "channel should enter command-started state");

    SshChannelCloseMessage close_msg;
    close_msg.recipient_channel = 57;
    auto close_payload_buf = SshMessageCodec::encode_channel_close(close_msg);
    auto close_span = close_payload_buf.readable_span();
    std::vector<uint8_t> close_payload(
        reinterpret_cast<const uint8_t *>(close_span.data()),
        reinterpret_cast<const uint8_t *>(close_span.data()) + close_span.size());

    ExitSignalHandler handler;
    session.set_state(SshSession::State::active);
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_CLOSE, close_payload, &handler);

    auto outgoing = session.drain_outgoing();
    TEST_ASSERT(outgoing.size() == 2,
                "channel close for command channel should enqueue exit notification then channel-close");

    auto first_span = outgoing[0].readable_span();
    auto second_span = outgoing[1].readable_span();
    auto first_req = SshMessageCodec::decode_channel_request(
        reinterpret_cast<const uint8_t *>(first_span.data()), first_span.size());
    TEST_ASSERT(first_req.has_value(), "first response should decode as channel request");
    TEST_ASSERT(first_req->request_type == "exit-signal", "first response should be exit-signal");

    size_t offset = 0;
    auto signal_name = SshMessageCodec::read_string(first_req->request_specific_data.data(),
                                                    first_req->request_specific_data.size(), offset);
    TEST_ASSERT(signal_name.has_value() && *signal_name == "TERM", "exit-signal should carry signal name");
    auto decoded_close = SshMessageCodec::decode_channel_close(
        reinterpret_cast<const uint8_t *>(second_span.data()), second_span.size());
    TEST_ASSERT(decoded_close.has_value(), "second response should decode as channel-close");
    return true;
}

bool test_session_dispatch_repeated_channel_close_emits_exit_only_once()
{
    SshSession session(1004, nullptr);
    auto &mgr = session.connection_manager();
    auto *channel = mgr.create_channel(SSH_CHANNEL_SESSION, 58, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");
    TEST_ASSERT(channel->mark_command_started(), "channel should enter command-started state");

    SshChannelCloseMessage close_msg;
    close_msg.recipient_channel = 58;
    auto close_payload_buf = SshMessageCodec::encode_channel_close(close_msg);
    auto close_span = close_payload_buf.readable_span();
    std::vector<uint8_t> close_payload(
        reinterpret_cast<const uint8_t *>(close_span.data()),
        reinterpret_cast<const uint8_t *>(close_span.data()) + close_span.size());

    session.set_state(SshSession::State::active);
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_CLOSE, close_payload, nullptr);
    auto first_out = session.drain_outgoing();
    TEST_ASSERT(first_out.size() == 2, "first close should emit exit-status and channel-close");

    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_CLOSE, close_payload, nullptr);
    auto second_out = session.drain_outgoing();
    TEST_ASSERT(second_out.empty(), "repeated close after channel removal should not emit extra packets");
    return true;
}

bool test_env_and_pty_rejected_after_command_started()
{
    class AcceptingHandler final : public SshHandler
    {
    public:
        bool on_exec_request(SshSession *session,
                             SshChannel *channel,
                             const std::string &command) override
        {
            (void)session;
            (void)channel;
            (void)command;
            return true;
        }

        bool on_env_request(SshSession *session,
                            SshChannel *channel,
                            const std::string &name,
                            const std::string &value) override
        {
            (void)session;
            (void)channel;
            (void)name;
            (void)value;
            return true;
        }

        bool on_pty_request(SshSession *session,
                            SshChannel *channel,
                            const std::string &term,
                            uint32_t width, uint32_t height,
                            uint32_t pixel_width, uint32_t pixel_height,
                            const std::vector<uint8_t> &modes) override
        {
            (void)session;
            (void)channel;
            (void)term;
            (void)width;
            (void)height;
            (void)pixel_width;
            (void)pixel_height;
            (void)modes;
            return true;
        }
    };

    SshConnectionManager mgr(nullptr);
    auto *channel = mgr.create_channel(SSH_CHANNEL_SESSION, 59, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");

    AcceptingHandler handler;

    SshChannelRequestMessage exec_msg;
    exec_msg.recipient_channel = 59;
    exec_msg.request_type = "exec";
    exec_msg.want_reply = true;
    exec_msg.request_specific_data = encode_string_payload("echo ok");
    auto exec_reply = mgr.handle_channel_request(exec_msg, &handler);
    auto exec_span = exec_reply.readable_span();
    TEST_ASSERT(!exec_span.empty(), "exec should reply");
    TEST_ASSERT(exec_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_SUCCESS),
                "exec should succeed first");

    yuan::buffer::ByteBuffer env_payload;
    SshMessageCodec::write_string(env_payload, "A");
    SshMessageCodec::write_string(env_payload, "1");
    auto env_span = env_payload.readable_span();
    SshChannelRequestMessage env_msg;
    env_msg.recipient_channel = 59;
    env_msg.request_type = "env";
    env_msg.want_reply = true;
    env_msg.request_specific_data.assign(
        reinterpret_cast<const uint8_t *>(env_span.data()),
        reinterpret_cast<const uint8_t *>(env_span.data()) + env_span.size());

    auto env_reply = mgr.handle_channel_request(env_msg, &handler);
    auto env_reply_span = env_reply.readable_span();
    TEST_ASSERT(!env_reply_span.empty(), "env after exec should reply");
    TEST_ASSERT(env_reply_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_FAILURE),
                "env must be rejected after command starts");

    yuan::buffer::ByteBuffer pty_payload;
    SshMessageCodec::write_string(pty_payload, "xterm");
    SshMessageCodec::write_uint32(pty_payload, 80);
    SshMessageCodec::write_uint32(pty_payload, 24);
    SshMessageCodec::write_uint32(pty_payload, 640);
    SshMessageCodec::write_uint32(pty_payload, 480);
    SshMessageCodec::write_string(pty_payload, std::string());
    auto pty_span = pty_payload.readable_span();
    SshChannelRequestMessage pty_msg;
    pty_msg.recipient_channel = 59;
    pty_msg.request_type = "pty-req";
    pty_msg.want_reply = true;
    pty_msg.request_specific_data.assign(
        reinterpret_cast<const uint8_t *>(pty_span.data()),
        reinterpret_cast<const uint8_t *>(pty_span.data()) + pty_span.size());

    auto pty_reply = mgr.handle_channel_request(pty_msg, &handler);
    auto pty_reply_span = pty_reply.readable_span();
    TEST_ASSERT(!pty_reply_span.empty(), "pty after exec should reply");
    TEST_ASSERT(pty_reply_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_FAILURE),
                "pty must be rejected after command starts");
    return true;
}

bool test_shell_success_then_rejects_other_command_requests()
{
    class ShellAcceptingHandler final : public SshHandler
    {
    public:
        bool on_shell_request(SshSession *session,
                              SshChannel *channel) override
        {
            (void)session;
            (void)channel;
            return true;
        }

        bool on_exec_request(SshSession *session,
                             SshChannel *channel,
                             const std::string &command) override
        {
            (void)session;
            (void)channel;
            (void)command;
            return true;
        }

        bool on_subsystem_request(SshSession *session,
                                  SshChannel *channel,
                                  const std::string &name) override
        {
            (void)session;
            (void)channel;
            (void)name;
            return true;
        }

        bool on_env_request(SshSession *session,
                            SshChannel *channel,
                            const std::string &name,
                            const std::string &value) override
        {
            (void)session;
            (void)channel;
            (void)name;
            (void)value;
            return true;
        }

        bool on_pty_request(SshSession *session,
                            SshChannel *channel,
                            const std::string &term,
                            uint32_t width, uint32_t height,
                            uint32_t pixel_width, uint32_t pixel_height,
                            const std::vector<uint8_t> &modes) override
        {
            (void)session;
            (void)channel;
            (void)term;
            (void)width;
            (void)height;
            (void)pixel_width;
            (void)pixel_height;
            (void)modes;
            return true;
        }
    };

    SshConnectionManager mgr(nullptr);
    auto *channel = mgr.create_channel(SSH_CHANNEL_SESSION, 60, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");

    ShellAcceptingHandler handler;

    SshChannelRequestMessage shell_msg;
    shell_msg.recipient_channel = 60;
    shell_msg.request_type = "shell";
    shell_msg.want_reply = true;
    auto shell_reply = mgr.handle_channel_request(shell_msg, &handler);
    auto shell_span = shell_reply.readable_span();
    TEST_ASSERT(!shell_span.empty(), "shell should reply");
    TEST_ASSERT(shell_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_SUCCESS),
                "shell should succeed first");

    SshChannelRequestMessage exec_msg;
    exec_msg.recipient_channel = 60;
    exec_msg.request_type = "exec";
    exec_msg.want_reply = true;
    exec_msg.request_specific_data = encode_string_payload("echo later");
    auto exec_reply = mgr.handle_channel_request(exec_msg, &handler);
    auto exec_span = exec_reply.readable_span();
    TEST_ASSERT(!exec_span.empty(), "exec after shell should reply");
    TEST_ASSERT(exec_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_FAILURE),
                "exec should be rejected after shell");

    SshChannelRequestMessage subsystem_msg;
    subsystem_msg.recipient_channel = 60;
    subsystem_msg.request_type = "subsystem";
    subsystem_msg.want_reply = true;
    subsystem_msg.request_specific_data = encode_string_payload("sftp");
    auto subsystem_reply = mgr.handle_channel_request(subsystem_msg, &handler);
    auto subsystem_span = subsystem_reply.readable_span();
    TEST_ASSERT(!subsystem_span.empty(), "subsystem after shell should reply");
    TEST_ASSERT(subsystem_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_FAILURE),
                "subsystem should be rejected after shell");

    yuan::buffer::ByteBuffer env_payload;
    SshMessageCodec::write_string(env_payload, "B");
    SshMessageCodec::write_string(env_payload, "2");
    auto env_payload_span = env_payload.readable_span();
    SshChannelRequestMessage env_msg;
    env_msg.recipient_channel = 60;
    env_msg.request_type = "env";
    env_msg.want_reply = true;
    env_msg.request_specific_data.assign(
        reinterpret_cast<const uint8_t *>(env_payload_span.data()),
        reinterpret_cast<const uint8_t *>(env_payload_span.data()) + env_payload_span.size());
    auto env_reply = mgr.handle_channel_request(env_msg, &handler);
    auto env_span = env_reply.readable_span();
    TEST_ASSERT(!env_span.empty(), "env after shell should reply");
    TEST_ASSERT(env_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_FAILURE),
                "env should be rejected after shell");

    yuan::buffer::ByteBuffer pty_payload;
    SshMessageCodec::write_string(pty_payload, "xterm");
    SshMessageCodec::write_uint32(pty_payload, 80);
    SshMessageCodec::write_uint32(pty_payload, 24);
    SshMessageCodec::write_uint32(pty_payload, 640);
    SshMessageCodec::write_uint32(pty_payload, 480);
    SshMessageCodec::write_string(pty_payload, std::string());
    auto pty_payload_span = pty_payload.readable_span();
    SshChannelRequestMessage pty_msg;
    pty_msg.recipient_channel = 60;
    pty_msg.request_type = "pty-req";
    pty_msg.want_reply = true;
    pty_msg.request_specific_data.assign(
        reinterpret_cast<const uint8_t *>(pty_payload_span.data()),
        reinterpret_cast<const uint8_t *>(pty_payload_span.data()) + pty_payload_span.size());
    auto pty_reply = mgr.handle_channel_request(pty_msg, &handler);
    auto pty_span = pty_reply.readable_span();
    TEST_ASSERT(!pty_span.empty(), "pty after shell should reply");
    TEST_ASSERT(pty_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_FAILURE),
                "pty should be rejected after shell");

    return true;
}

bool test_channel_request_non_open_state_reply_behavior()
{
    SshConnectionManager mgr(nullptr);
    auto *channel = mgr.create_channel(SSH_CHANNEL_SESSION, 61, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");

    channel->set_state(SshChannel::State::eof);

    SshChannelRequestMessage shell_with_reply;
    shell_with_reply.recipient_channel = 61;
    shell_with_reply.request_type = "shell";
    shell_with_reply.want_reply = true;
    auto reply = mgr.handle_channel_request(shell_with_reply, nullptr);
    auto reply_span = reply.readable_span();
    TEST_ASSERT(!reply_span.empty(), "non-open channel with want_reply should return response");
    TEST_ASSERT(reply_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_FAILURE),
                "non-open channel request should fail");

    SshChannelRequestMessage shell_no_reply = shell_with_reply;
    shell_no_reply.want_reply = false;
    auto no_reply = mgr.handle_channel_request(shell_no_reply, nullptr);
    auto no_reply_span = no_reply.readable_span();
    TEST_ASSERT(no_reply_span.empty(), "non-open channel without want_reply should return no response");
    return true;
}

bool test_subsystem_success_then_rejects_other_command_requests()
{
    class SubsystemAcceptingHandler final : public SshHandler
    {
    public:
        bool on_subsystem_request(SshSession *session,
                                  SshChannel *channel,
                                  const std::string &name) override
        {
            (void)session;
            (void)channel;
            return name == "custom-subsys";
        }

        bool on_exec_request(SshSession *session,
                             SshChannel *channel,
                             const std::string &command) override
        {
            (void)session;
            (void)channel;
            (void)command;
            return true;
        }

        bool on_shell_request(SshSession *session,
                              SshChannel *channel) override
        {
            (void)session;
            (void)channel;
            return true;
        }
    };

    SshConnectionManager mgr(nullptr);
    auto *channel = mgr.create_channel(SSH_CHANNEL_SESSION, 62, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");

    SubsystemAcceptingHandler handler;

    SshChannelRequestMessage subsystem_msg;
    subsystem_msg.recipient_channel = 62;
    subsystem_msg.request_type = "subsystem";
    subsystem_msg.want_reply = true;
    subsystem_msg.request_specific_data = encode_string_payload("custom-subsys");
    auto subsystem_reply = mgr.handle_channel_request(subsystem_msg, &handler);
    auto subsystem_span = subsystem_reply.readable_span();
    TEST_ASSERT(!subsystem_span.empty(), "subsystem should reply");
    TEST_ASSERT(subsystem_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_SUCCESS),
                "subsystem should succeed first");

    SshChannelRequestMessage exec_msg;
    exec_msg.recipient_channel = 62;
    exec_msg.request_type = "exec";
    exec_msg.want_reply = true;
    exec_msg.request_specific_data = encode_string_payload("echo x");
    auto exec_reply = mgr.handle_channel_request(exec_msg, &handler);
    auto exec_span = exec_reply.readable_span();
    TEST_ASSERT(!exec_span.empty(), "exec after subsystem should reply");
    TEST_ASSERT(exec_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_FAILURE),
                "exec should be rejected after subsystem starts");

    SshChannelRequestMessage shell_msg;
    shell_msg.recipient_channel = 62;
    shell_msg.request_type = "shell";
    shell_msg.want_reply = true;
    auto shell_reply = mgr.handle_channel_request(shell_msg, &handler);
    auto shell_span = shell_reply.readable_span();
    TEST_ASSERT(!shell_span.empty(), "shell after subsystem should reply");
    TEST_ASSERT(shell_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_FAILURE),
                "shell should be rejected after subsystem starts");
    return true;
}

bool test_session_dispatch_channel_close_uses_handler_exit_status_value()
{
    class ExitStatusHandler final : public SshHandler
    {
    public:
        SshCommandExitInfo on_command_exit(SshSession *session,
                                           SshChannel *channel) override
        {
            (void)session;
            (void)channel;
            SshCommandExitInfo info;
            info.use_signal = false;
            info.exit_status = 42;
            return info;
        }
    };

    SshSession session(1005, nullptr);
    auto &mgr = session.connection_manager();
    auto *channel = mgr.create_channel(SSH_CHANNEL_SESSION, 63, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");
    TEST_ASSERT(channel->mark_command_started(), "channel should enter command-started state");

    SshChannelCloseMessage close_msg;
    close_msg.recipient_channel = 63;
    auto close_payload_buf = SshMessageCodec::encode_channel_close(close_msg);
    auto close_span = close_payload_buf.readable_span();
    std::vector<uint8_t> close_payload(
        reinterpret_cast<const uint8_t *>(close_span.data()),
        reinterpret_cast<const uint8_t *>(close_span.data()) + close_span.size());

    ExitStatusHandler handler;
    session.set_state(SshSession::State::active);
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_CLOSE, close_payload, &handler);

    auto outgoing = session.drain_outgoing();
    TEST_ASSERT(outgoing.size() == 2, "close should emit exit-status and channel-close");

    auto first_span = outgoing[0].readable_span();
    auto first_req = SshMessageCodec::decode_channel_request(
        reinterpret_cast<const uint8_t *>(first_span.data()), first_span.size());
    TEST_ASSERT(first_req.has_value(), "first response should decode as channel request");
    TEST_ASSERT(first_req->request_type == "exit-status", "first response should be exit-status");

    size_t offset = 0;
    uint32_t status = SshMessageCodec::read_uint32(first_req->request_specific_data.data(),
                                                   first_req->request_specific_data.size(), offset);
    TEST_ASSERT(status == 42, "exit-status should carry handler-provided status code");
    TEST_ASSERT(offset == first_req->request_specific_data.size(), "exit-status payload should contain only status");
    return true;
}

bool test_session_dispatch_channel_close_exit_signal_payload_fields()
{
    class RichExitSignalHandler final : public SshHandler
    {
    public:
        SshCommandExitInfo on_command_exit(SshSession *session,
                                           SshChannel *channel) override
        {
            (void)session;
            (void)channel;
            SshCommandExitInfo info;
            info.use_signal = true;
            info.signal_name = "KILL";
            info.core_dumped = true;
            info.error_message = "killed by test";
            info.language_tag = "en-US";
            return info;
        }
    };

    SshSession session(1006, nullptr);
    auto &mgr = session.connection_manager();
    auto *channel = mgr.create_channel(SSH_CHANNEL_SESSION, 64, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");
    TEST_ASSERT(channel->mark_command_started(), "channel should enter command-started state");

    SshChannelCloseMessage close_msg;
    close_msg.recipient_channel = 64;
    auto close_payload_buf = SshMessageCodec::encode_channel_close(close_msg);
    auto close_span = close_payload_buf.readable_span();
    std::vector<uint8_t> close_payload(
        reinterpret_cast<const uint8_t *>(close_span.data()),
        reinterpret_cast<const uint8_t *>(close_span.data()) + close_span.size());

    RichExitSignalHandler handler;
    session.set_state(SshSession::State::active);
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_CLOSE, close_payload, &handler);

    auto outgoing = session.drain_outgoing();
    TEST_ASSERT(outgoing.size() == 2, "close should emit exit-signal and channel-close");

    auto req_span = outgoing[0].readable_span();
    auto req = SshMessageCodec::decode_channel_request(
        reinterpret_cast<const uint8_t *>(req_span.data()), req_span.size());
    TEST_ASSERT(req.has_value(), "first response should decode as channel request");
    TEST_ASSERT(req->request_type == "exit-signal", "first response should be exit-signal");

    size_t offset = 0;
    auto signal_name = SshMessageCodec::read_string(req->request_specific_data.data(),
                                                    req->request_specific_data.size(), offset);
    TEST_ASSERT(signal_name.has_value() && *signal_name == "KILL", "exit-signal should carry configured signal");
    bool core_dumped = SshMessageCodec::read_boolean(req->request_specific_data.data(),
                                                     req->request_specific_data.size(), offset);
    TEST_ASSERT(core_dumped, "exit-signal should carry configured core-dump flag");
    auto error_message = SshMessageCodec::read_string(req->request_specific_data.data(),
                                                      req->request_specific_data.size(), offset);
    TEST_ASSERT(error_message.has_value() && *error_message == "killed by test",
                "exit-signal should carry configured error message");
    auto language_tag = SshMessageCodec::read_string(req->request_specific_data.data(),
                                                     req->request_specific_data.size(), offset);
    TEST_ASSERT(language_tag.has_value() && *language_tag == "en-US",
                "exit-signal should carry configured language tag");
    TEST_ASSERT(offset == req->request_specific_data.size(),
                "exit-signal payload should be fully consumed");
    return true;
}

bool test_channel_request_sequence_matrix_command_then_exec_shell_subsystem_fail()
{
    class AcceptingHandler final : public SshHandler
    {
    public:
        bool on_exec_request(SshSession *session,
                             SshChannel *channel,
                             const std::string &command) override
        {
            (void)session;
            (void)channel;
            (void)command;
            return true;
        }

        bool on_shell_request(SshSession *session,
                              SshChannel *channel) override
        {
            (void)session;
            (void)channel;
            return true;
        }

        bool on_subsystem_request(SshSession *session,
                                  SshChannel *channel,
                                  const std::string &name) override
        {
            (void)session;
            (void)channel;
            (void)name;
            return true;
        }
    };

    SshConnectionManager mgr(nullptr);
    auto *channel = mgr.create_channel(SSH_CHANNEL_SESSION, 65, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");

    AcceptingHandler handler;

    SshChannelRequestMessage first;
    first.recipient_channel = 65;
    first.request_type = "exec";
    first.want_reply = true;
    first.request_specific_data = encode_string_payload("echo first");
    auto first_reply = mgr.handle_channel_request(first, &handler);
    auto first_span = first_reply.readable_span();
    TEST_ASSERT(!first_span.empty(), "first request should reply");
    TEST_ASSERT(first_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_SUCCESS),
                "first exec should succeed");

    const std::array<std::string, 3> followups = { "exec", "shell", "subsystem" };
    for (const auto & req_type : followups) {
        SshChannelRequestMessage msg;
        msg.recipient_channel = 65;
        msg.request_type = req_type;
        msg.want_reply = true;
        if (req_type == "exec") {
            msg.request_specific_data = encode_string_payload("echo second");
        } else if (req_type == "subsystem") {
            msg.request_specific_data = encode_string_payload("sftp");
        }

        auto reply = mgr.handle_channel_request(msg, &handler);
        auto span = reply.readable_span();
        TEST_ASSERT(!span.empty(), "follow-up command request should reply");
        TEST_ASSERT(span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_FAILURE),
                    "follow-up command request should be rejected after first command starts");
    }
    return true;
}

bool test_terminal_session_state_records_accepted_pty_and_shell_flags()
{
    class InteractiveHandler final : public SshHandler
    {
    public:
        bool on_pty_request(SshSession *session,
                            SshChannel *channel,
                            const std::string &term,
                            uint32_t width, uint32_t height,
                            uint32_t pixel_width, uint32_t pixel_height,
                            const std::vector<uint8_t> &modes) override
        {
            (void)session;
            (void)channel;
            (void)term;
            (void)width;
            (void)height;
            (void)pixel_width;
            (void)pixel_height;
            (void)modes;
            return true;
        }

        bool on_shell_request(SshSession *session,
                              SshChannel *channel) override
        {
            (void)session;
            (void)channel;
            return true;
        }
    };

    SshConnectionManager mgr(nullptr);
    auto *channel = mgr.create_channel(SSH_CHANNEL_SESSION, 66, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created");

    InteractiveHandler handler;

    yuan::buffer::ByteBuffer pty_payload;
    SshMessageCodec::write_string(pty_payload, "xterm-256color");
    SshMessageCodec::write_uint32(pty_payload, 132);
    SshMessageCodec::write_uint32(pty_payload, 43);
    SshMessageCodec::write_uint32(pty_payload, 1200);
    SshMessageCodec::write_uint32(pty_payload, 860);
    SshMessageCodec::write_string(pty_payload, std::string("\x01\x00\x00\x00\x00\x00", 6));

    auto pty_span = pty_payload.readable_span();
    SshChannelRequestMessage pty_msg;
    pty_msg.recipient_channel = 66;
    pty_msg.request_type = "pty-req";
    pty_msg.want_reply = true;
    pty_msg.request_specific_data.assign(
        reinterpret_cast<const uint8_t *>(pty_span.data()),
        reinterpret_cast<const uint8_t *>(pty_span.data()) + pty_span.size());

    auto pty_reply = mgr.handle_channel_request(pty_msg, &handler);
    auto pty_reply_span = pty_reply.readable_span();
    TEST_ASSERT(!pty_reply_span.empty(), "pty request should reply");
    TEST_ASSERT(pty_reply_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_SUCCESS),
                "pty request should be accepted by handler");

    const auto &state_after_pty = channel->terminal_session_state();
    TEST_ASSERT(state_after_pty.has_pty_request, "terminal state should mark accepted pty request");
    TEST_ASSERT(state_after_pty.spec.term_env == "xterm-256color", "terminal term should be persisted");
    TEST_ASSERT(state_after_pty.spec.width == 132 && state_after_pty.spec.height == 43,
                "terminal character dimensions should be persisted");
    TEST_ASSERT(state_after_pty.spec.pixel_width == 1200 && state_after_pty.spec.pixel_height == 860,
                "terminal pixel dimensions should be persisted");
    TEST_ASSERT(state_after_pty.spec.terminal_modes.size() == 6,
                "terminal modes should be persisted");

    SshChannelRequestMessage shell_msg;
    shell_msg.recipient_channel = 66;
    shell_msg.request_type = "shell";
    shell_msg.want_reply = true;
    auto shell_reply = mgr.handle_channel_request(shell_msg, &handler);
    auto shell_reply_span = shell_reply.readable_span();
    TEST_ASSERT(!shell_reply_span.empty(), "shell request should reply");
    TEST_ASSERT(shell_reply_span.data()[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_SUCCESS),
                "shell request should be accepted by handler");

    TEST_ASSERT(channel->terminal_session_state().interactive_shell_requested,
                "terminal state should mark interactive shell requested");
    return true;
}

bool test_pty_backend_prepare_and_shutdown_lifecycle()
{
    SshPtyProcess process;
    SshTerminalSpec spec;
    spec.term_env = "xterm-256color";
    spec.width = 120;
    spec.height = 40;
    spec.pixel_width = 1200;
    spec.pixel_height = 800;

    std::string err;
    const bool ok = process.prepare(spec, &err);
#if defined(_WIN32)
    TEST_ASSERT(!ok, "pty prepare should not be ready on windows placeholder");
    TEST_ASSERT(!process.ready(), "pty process should remain not ready on windows placeholder");
#else
    TEST_ASSERT(ok, "pty prepare should succeed on unix-like platforms with openpty");
    TEST_ASSERT(process.ready(), "pty process should be ready after prepare");
    TEST_ASSERT(process.backend().master_fd() >= 0, "pty master fd should be valid");
    TEST_ASSERT(process.backend().slave_fd() >= 0, "pty slave fd should be valid");
#endif

    process.shutdown();
    TEST_ASSERT(!process.ready(), "pty process should not be ready after shutdown");
    return true;
}

bool test_pty_process_launch_shell_and_capture_output()
{
#if defined(_WIN32)
    return true;
#else
    SshPtyProcess process;
    SshTerminalSpec spec;
    spec.term_env = "xterm";
    spec.width = 80;
    spec.height = 24;

    std::string err;
    TEST_ASSERT(process.prepare(spec, &err), "pty process should prepare before launch shell");
    TEST_ASSERT(process.launch_shell("printf shell-bridge-ok", false, &err),
                "pty process should launch non-interactive shell command");

    bool saw_output = false;
    for (int i = 0; i < 20; ++i) {
        std::vector<uint8_t> chunk;
        if (process.read_output(&chunk, 4096) && !chunk.empty()) {
            const std::string text(chunk.begin(), chunk.end());
            if (text.find("shell-bridge-ok") != std::string::npos) {
                saw_output = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TEST_ASSERT(saw_output, "pty process should expose shell output via master side read");

    bool saw_exit = false;
    SshPtyExitState state;
    for (int i = 0; i < 50; ++i) {
        if (process.poll_exit(&state)) {
            saw_exit = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TEST_ASSERT(saw_exit, "pty process should report child exit");
    TEST_ASSERT(state.exited, "pty process child should exit normally for printf command");
    TEST_ASSERT(state.exit_code == 0, "pty process child should exit with status 0");

    process.shutdown();
    TEST_ASSERT(!process.ready(), "pty process should not be ready after shutdown");
    return true;
#endif
}

bool test_session_pty_bridge_shell_data_and_exit_sequence()
{
#if defined(_WIN32)
    return true;
#else
    class ShellAcceptHandler final : public SshHandler
    {
    public:
        bool on_shell_request(SshSession *session,
                              SshChannel *channel) override
        {
            (void)session;
            (void)channel;
            return true;
        }

        bool on_pty_request(SshSession *session,
                            SshChannel *channel,
                            const std::string &term,
                            uint32_t width, uint32_t height,
                            uint32_t pixel_width, uint32_t pixel_height,
                            const std::vector<uint8_t> &modes) override
        {
            (void)session;
            (void)channel;
            (void)term;
            (void)width;
            (void)height;
            (void)pixel_width;
            (void)pixel_height;
            (void)modes;
            return true;
        }
    };

    SshSession session(2002, nullptr);
    session.set_state(SshSession::State::active);
    auto *channel = session.connection_manager().create_channel(
        SSH_CHANNEL_SESSION, 89, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created for session PTY bridge test");

    ShellAcceptHandler handler;

    yuan::buffer::ByteBuffer pty_payload;
    SshMessageCodec::write_string(pty_payload, "xterm");
    SshMessageCodec::write_uint32(pty_payload, 100);
    SshMessageCodec::write_uint32(pty_payload, 30);
    SshMessageCodec::write_uint32(pty_payload, 1000);
    SshMessageCodec::write_uint32(pty_payload, 700);
    SshMessageCodec::write_string(pty_payload, std::string());
    auto pty_span = pty_payload.readable_span();

    SshChannelRequestMessage pty_req;
    pty_req.recipient_channel = 89;
    pty_req.request_type = "pty-req";
    pty_req.want_reply = true;
    pty_req.request_specific_data.assign(
        reinterpret_cast<const uint8_t *>(pty_span.data()),
        reinterpret_cast<const uint8_t *>(pty_span.data()) + pty_span.size());
    auto pty_packet = SshMessageCodec::encode_channel_request(pty_req);
    auto pty_packet_span = pty_packet.readable_span();
    std::vector<uint8_t> pty_raw(
        reinterpret_cast<const uint8_t *>(pty_packet_span.data()),
        reinterpret_cast<const uint8_t *>(pty_packet_span.data()) + pty_packet_span.size());
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_REQUEST, pty_raw, &handler);

    SshChannelRequestMessage shell_req;
    shell_req.recipient_channel = 89;
    shell_req.request_type = "shell";
    shell_req.want_reply = true;
    auto shell_packet = SshMessageCodec::encode_channel_request(shell_req);
    auto shell_packet_span = shell_packet.readable_span();
    std::vector<uint8_t> shell_raw(
        reinterpret_cast<const uint8_t *>(shell_packet_span.data()),
        reinterpret_cast<const uint8_t *>(shell_packet_span.data()) + shell_packet_span.size());
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_REQUEST, shell_raw, &handler);

    TEST_ASSERT(session.has_pty_process(89), "shell with accepted pty should start PTY process bridge");
    TEST_ASSERT(channel->terminal_session_state().pty_bridge_active,
                "channel terminal state should mark PTY bridge active");

    const std::string script = "printf bridge-data; exit\n";
    SshChannelDataMessage data_msg;
    data_msg.recipient_channel = 89;
    data_msg.data.assign(script.begin(), script.end());
    auto data_packet = SshMessageCodec::encode_channel_data(data_msg);
    auto data_span = data_packet.readable_span();
    std::vector<uint8_t> data_raw(
        reinterpret_cast<const uint8_t *>(data_span.data()),
        reinterpret_cast<const uint8_t *>(data_span.data()) + data_span.size());
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_DATA, data_raw, &handler);

    bool saw_bridge_output = false;
    bool saw_exit_status = false;
    bool saw_close = false;
    for (int i = 0; i < 80; ++i) {
        session.pump_pty_once(89, &handler);
        auto out = session.drain_outgoing();
        for (auto &buf : out) {
            auto span = buf.readable_span();
            if (span.empty()) {
                continue;
            }
            const auto *raw = reinterpret_cast<const uint8_t *>(span.data());
            const auto type = static_cast<SshMessageType>(raw[0]);
            if (type == SshMessageType::SSH_MSG_CHANNEL_DATA) {
                auto decoded = SshMessageCodec::decode_channel_data(raw, span.size());
                if (decoded) {
                    const std::string text(decoded->data.begin(), decoded->data.end());
                    if (text.find("bridge-data") != std::string::npos) {
                        saw_bridge_output = true;
                    }
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_REQUEST) {
                auto decoded = SshMessageCodec::decode_channel_request(raw, span.size());
                if (decoded && decoded->request_type == "exit-status") {
                    saw_exit_status = true;
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_CLOSE) {
                saw_close = true;
            }
        }

        if (saw_bridge_output && saw_exit_status && saw_close) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    TEST_ASSERT(saw_bridge_output, "PTY bridge should forward shell output as channel-data");
    TEST_ASSERT(saw_exit_status, "PTY bridge should emit exit-status when shell exits");
    TEST_ASSERT(saw_close, "PTY bridge should emit channel-close when shell exits");
    return true;
#endif
}

bool test_session_pty_bridge_window_change_affects_shell_stty_size()
{
#if defined(_WIN32)
    return true;
#else
    class ShellAcceptHandler final : public SshHandler
    {
    public:
        bool on_shell_request(SshSession *session,
                              SshChannel *channel) override
        {
            (void)session;
            (void)channel;
            return true;
        }

        bool on_pty_request(SshSession *session,
                            SshChannel *channel,
                            const std::string &term,
                            uint32_t width, uint32_t height,
                            uint32_t pixel_width, uint32_t pixel_height,
                            const std::vector<uint8_t> &modes) override
        {
            (void)session;
            (void)channel;
            (void)term;
            (void)width;
            (void)height;
            (void)pixel_width;
            (void)pixel_height;
            (void)modes;
            return true;
        }
    };

    SshSession session(2003, nullptr);
    session.set_state(SshSession::State::active);
    auto *channel = session.connection_manager().create_channel(
        SSH_CHANNEL_SESSION, 90, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created for window-change bridge test");

    ShellAcceptHandler handler;

    yuan::buffer::ByteBuffer pty_payload;
    SshMessageCodec::write_string(pty_payload, "xterm");
    SshMessageCodec::write_uint32(pty_payload, 100);
    SshMessageCodec::write_uint32(pty_payload, 30);
    SshMessageCodec::write_uint32(pty_payload, 1000);
    SshMessageCodec::write_uint32(pty_payload, 700);
    SshMessageCodec::write_string(pty_payload, std::string());
    auto pty_span = pty_payload.readable_span();
    SshChannelRequestMessage pty_req;
    pty_req.recipient_channel = 90;
    pty_req.request_type = "pty-req";
    pty_req.want_reply = true;
    pty_req.request_specific_data.assign(
        reinterpret_cast<const uint8_t *>(pty_span.data()),
        reinterpret_cast<const uint8_t *>(pty_span.data()) + pty_span.size());
    auto pty_packet = SshMessageCodec::encode_channel_request(pty_req);
    auto pty_packet_span = pty_packet.readable_span();
    std::vector<uint8_t> pty_raw(
        reinterpret_cast<const uint8_t *>(pty_packet_span.data()),
        reinterpret_cast<const uint8_t *>(pty_packet_span.data()) + pty_packet_span.size());
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_REQUEST, pty_raw, &handler);

    SshChannelRequestMessage shell_req;
    shell_req.recipient_channel = 90;
    shell_req.request_type = "shell";
    shell_req.want_reply = true;
    auto shell_packet = SshMessageCodec::encode_channel_request(shell_req);
    auto shell_packet_span = shell_packet.readable_span();
    std::vector<uint8_t> shell_raw(
        reinterpret_cast<const uint8_t *>(shell_packet_span.data()),
        reinterpret_cast<const uint8_t *>(shell_packet_span.data()) + shell_packet_span.size());
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_REQUEST, shell_raw, &handler);
    TEST_ASSERT(session.has_pty_process(90), "shell bridge should start PTY process");

    yuan::buffer::ByteBuffer wc_payload;
    SshMessageCodec::write_uint32(wc_payload, 101);
    SshMessageCodec::write_uint32(wc_payload, 41);
    SshMessageCodec::write_uint32(wc_payload, 1300);
    SshMessageCodec::write_uint32(wc_payload, 900);
    auto wc_span = wc_payload.readable_span();
    SshChannelRequestMessage wc_req;
    wc_req.recipient_channel = 90;
    wc_req.request_type = "window-change";
    wc_req.want_reply = true;
    wc_req.request_specific_data.assign(
        reinterpret_cast<const uint8_t *>(wc_span.data()),
        reinterpret_cast<const uint8_t *>(wc_span.data()) + wc_span.size());
    auto wc_packet = SshMessageCodec::encode_channel_request(wc_req);
    auto wc_packet_span = wc_packet.readable_span();
    std::vector<uint8_t> wc_raw(
        reinterpret_cast<const uint8_t *>(wc_packet_span.data()),
        reinterpret_cast<const uint8_t *>(wc_packet_span.data()) + wc_packet_span.size());
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_REQUEST, wc_raw, &handler);

    const std::string script = "stty size; exit\n";
    SshChannelDataMessage data_msg;
    data_msg.recipient_channel = 90;
    data_msg.data.assign(script.begin(), script.end());
    auto data_packet = SshMessageCodec::encode_channel_data(data_msg);
    auto data_span = data_packet.readable_span();
    std::vector<uint8_t> data_raw(
        reinterpret_cast<const uint8_t *>(data_span.data()),
        reinterpret_cast<const uint8_t *>(data_span.data()) + data_span.size());
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_DATA, data_raw, &handler);

    bool saw_size = false;
    for (int i = 0; i < 80; ++i) {
        session.pump_pty_once(90, &handler);
        auto out = session.drain_outgoing();
        for (auto &buf : out) {
            auto span = buf.readable_span();
            if (span.empty()) {
                continue;
            }
            const auto *raw = reinterpret_cast<const uint8_t *>(span.data());
            if (static_cast<SshMessageType>(raw[0]) == SshMessageType::SSH_MSG_CHANNEL_DATA) {
                auto decoded = SshMessageCodec::decode_channel_data(raw, span.size());
                if (decoded) {
                    const std::string text(decoded->data.begin(), decoded->data.end());
                    if (text.find("41 101") != std::string::npos) {
                        saw_size = true;
                    }
                }
            }
        }
        if (saw_size) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    TEST_ASSERT(saw_size, "window-change should affect PTY size observed by stty");
    return true;
#endif
}

bool test_session_pty_bridge_signal_terminates_shell_child()
{
#if defined(_WIN32)
    return true;
#else
    class ShellAcceptHandler final : public SshHandler
    {
    public:
        bool on_shell_request(SshSession *session,
                              SshChannel *channel) override
        {
            (void)session;
            (void)channel;
            return true;
        }

        bool on_pty_request(SshSession *session,
                            SshChannel *channel,
                            const std::string &term,
                            uint32_t width, uint32_t height,
                            uint32_t pixel_width, uint32_t pixel_height,
                            const std::vector<uint8_t> &modes) override
        {
            (void)session;
            (void)channel;
            (void)term;
            (void)width;
            (void)height;
            (void)pixel_width;
            (void)pixel_height;
            (void)modes;
            return true;
        }
    };

    SshSession session(2004, nullptr);
    session.set_state(SshSession::State::active);
    auto *channel = session.connection_manager().create_channel(
        SSH_CHANNEL_SESSION, 91, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created for signal bridge test");

    ShellAcceptHandler handler;

    yuan::buffer::ByteBuffer pty_payload;
    SshMessageCodec::write_string(pty_payload, "xterm");
    SshMessageCodec::write_uint32(pty_payload, 100);
    SshMessageCodec::write_uint32(pty_payload, 30);
    SshMessageCodec::write_uint32(pty_payload, 1000);
    SshMessageCodec::write_uint32(pty_payload, 700);
    SshMessageCodec::write_string(pty_payload, std::string());
    auto pty_span = pty_payload.readable_span();
    SshChannelRequestMessage pty_req;
    pty_req.recipient_channel = 91;
    pty_req.request_type = "pty-req";
    pty_req.want_reply = true;
    pty_req.request_specific_data.assign(
        reinterpret_cast<const uint8_t *>(pty_span.data()),
        reinterpret_cast<const uint8_t *>(pty_span.data()) + pty_span.size());
    auto pty_packet = SshMessageCodec::encode_channel_request(pty_req);
    auto pty_packet_span = pty_packet.readable_span();
    std::vector<uint8_t> pty_raw(
        reinterpret_cast<const uint8_t *>(pty_packet_span.data()),
        reinterpret_cast<const uint8_t *>(pty_packet_span.data()) + pty_packet_span.size());
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_REQUEST, pty_raw, &handler);

    SshChannelRequestMessage shell_req;
    shell_req.recipient_channel = 91;
    shell_req.request_type = "shell";
    shell_req.want_reply = true;
    auto shell_packet = SshMessageCodec::encode_channel_request(shell_req);
    auto shell_packet_span = shell_packet.readable_span();
    std::vector<uint8_t> shell_raw(
        reinterpret_cast<const uint8_t *>(shell_packet_span.data()),
        reinterpret_cast<const uint8_t *>(shell_packet_span.data()) + shell_packet_span.size());
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_REQUEST, shell_raw, &handler);
    TEST_ASSERT(session.has_pty_process(91), "shell bridge should start PTY process");

    const std::string sleep_cmd = "sleep 30\n";
    SshChannelDataMessage sleep_msg;
    sleep_msg.recipient_channel = 91;
    sleep_msg.data.assign(sleep_cmd.begin(), sleep_cmd.end());
    auto sleep_packet = SshMessageCodec::encode_channel_data(sleep_msg);
    auto sleep_span = sleep_packet.readable_span();
    std::vector<uint8_t> sleep_raw(
        reinterpret_cast<const uint8_t *>(sleep_span.data()),
        reinterpret_cast<const uint8_t *>(sleep_span.data()) + sleep_span.size());
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_DATA, sleep_raw, &handler);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    SshChannelRequestMessage sig_req;
    sig_req.recipient_channel = 91;
    sig_req.request_type = "signal";
    sig_req.want_reply = true;
    sig_req.request_specific_data = encode_string_payload("KILL");
    auto sig_packet = SshMessageCodec::encode_channel_request(sig_req);
    auto sig_span = sig_packet.readable_span();
    std::vector<uint8_t> sig_raw(
        reinterpret_cast<const uint8_t *>(sig_span.data()),
        reinterpret_cast<const uint8_t *>(sig_span.data()) + sig_span.size());
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_REQUEST, sig_raw, &handler);

    bool saw_exit_signal = false;
    bool saw_close = false;
    for (int i = 0; i < 120; ++i) {
        session.pump_pty_once(91, &handler);
        auto out = session.drain_outgoing();
        for (auto &buf : out) {
            auto span = buf.readable_span();
            if (span.empty()) {
                continue;
            }
            const auto *raw = reinterpret_cast<const uint8_t *>(span.data());
            const auto type = static_cast<SshMessageType>(raw[0]);
            if (type == SshMessageType::SSH_MSG_CHANNEL_REQUEST) {
                auto decoded = SshMessageCodec::decode_channel_request(raw, span.size());
                if (decoded && decoded->request_type == "exit-signal") {
                    saw_exit_signal = true;
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_CLOSE) {
                saw_close = true;
            }
        }
        if (saw_exit_signal && saw_close) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    TEST_ASSERT(saw_exit_signal, "signal forwarding should lead to exit-signal notification");
    TEST_ASSERT(saw_close, "signal forwarding should eventually close channel");
    return true;
#endif
}

bool test_session_pty_bridge_exec_with_pty_outputs_and_exits()
{
#if defined(_WIN32)
    return true;
#else
    class ExecAcceptHandler final : public SshHandler
    {
    public:
        bool enable_builtin_exec_bridge() const override
        {
            return true;
        }

        bool on_exec_request(SshSession *session,
                             SshChannel *channel,
                             const std::string &command) override
        {
            (void)session;
            (void)channel;
            (void)command;
            return true;
        }

        bool on_pty_request(SshSession *session,
                            SshChannel *channel,
                            const std::string &term,
                            uint32_t width, uint32_t height,
                            uint32_t pixel_width, uint32_t pixel_height,
                            const std::vector<uint8_t> &modes) override
        {
            (void)session;
            (void)channel;
            (void)term;
            (void)width;
            (void)height;
            (void)pixel_width;
            (void)pixel_height;
            (void)modes;
            return true;
        }
    };

    SshSession session(2005, nullptr);
    session.set_state(SshSession::State::active);
    auto *channel = session.connection_manager().create_channel(
        SSH_CHANNEL_SESSION, 92, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created for exec PTY bridge test");

    ExecAcceptHandler handler;

    yuan::buffer::ByteBuffer pty_payload;
    SshMessageCodec::write_string(pty_payload, "xterm");
    SshMessageCodec::write_uint32(pty_payload, 100);
    SshMessageCodec::write_uint32(pty_payload, 30);
    SshMessageCodec::write_uint32(pty_payload, 1000);
    SshMessageCodec::write_uint32(pty_payload, 700);
    SshMessageCodec::write_string(pty_payload, std::string());
    auto pty_span = pty_payload.readable_span();
    SshChannelRequestMessage pty_req;
    pty_req.recipient_channel = 92;
    pty_req.request_type = "pty-req";
    pty_req.want_reply = true;
    pty_req.request_specific_data.assign(
        reinterpret_cast<const uint8_t *>(pty_span.data()),
        reinterpret_cast<const uint8_t *>(pty_span.data()) + pty_span.size());
    auto pty_packet = SshMessageCodec::encode_channel_request(pty_req);
    auto pty_packet_span = pty_packet.readable_span();
    std::vector<uint8_t> pty_raw(
        reinterpret_cast<const uint8_t *>(pty_packet_span.data()),
        reinterpret_cast<const uint8_t *>(pty_packet_span.data()) + pty_packet_span.size());
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_REQUEST, pty_raw, &handler);

    SshChannelRequestMessage exec_req;
    exec_req.recipient_channel = 92;
    exec_req.request_type = "exec";
    exec_req.want_reply = true;
    exec_req.request_specific_data = encode_string_payload("printf EXEC_BRIDGE_OK");
    auto exec_packet = SshMessageCodec::encode_channel_request(exec_req);
    auto exec_packet_span = exec_packet.readable_span();
    std::vector<uint8_t> exec_raw(
        reinterpret_cast<const uint8_t *>(exec_packet_span.data()),
        reinterpret_cast<const uint8_t *>(exec_packet_span.data()) + exec_packet_span.size());
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_REQUEST, exec_raw, &handler);

    TEST_ASSERT(session.has_pty_process(92), "exec with accepted pty should start PTY process bridge");

    bool saw_exec_output = false;
    bool saw_exit = false;
    for (int i = 0; i < 120; ++i) {
        session.pump_pty_once(92, &handler);
        auto out = session.drain_outgoing();
        for (auto &buf : out) {
            auto span = buf.readable_span();
            if (span.empty()) {
                continue;
            }
            const auto *raw = reinterpret_cast<const uint8_t *>(span.data());
            const auto type = static_cast<SshMessageType>(raw[0]);
            if (type == SshMessageType::SSH_MSG_CHANNEL_DATA) {
                auto decoded = SshMessageCodec::decode_channel_data(raw, span.size());
                if (decoded) {
                    const std::string text(decoded->data.begin(), decoded->data.end());
                    if (text.find("EXEC_BRIDGE_OK") != std::string::npos) {
                        saw_exec_output = true;
                    }
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_REQUEST) {
                auto decoded = SshMessageCodec::decode_channel_request(raw, span.size());
                if (decoded && (decoded->request_type == "exit-status" || decoded->request_type == "exit-signal")) {
                    saw_exit = true;
                }
            }
        }
        if (saw_exec_output && saw_exit) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    TEST_ASSERT(saw_exec_output, "exec PTY bridge should forward command output");
    TEST_ASSERT(saw_exit, "exec PTY bridge should emit exit notification");
    return true;
#endif
}

bool test_session_pty_bridge_exec_requires_handler_opt_in()
{
#if defined(_WIN32)
    return true;
#else
    class ExecAcceptNoBridgeHandler final : public SshHandler
    {
    public:
        bool on_exec_request(SshSession *session,
                             SshChannel *channel,
                             const std::string &command) override
        {
            (void)session;
            (void)channel;
            (void)command;
            return true;
        }

        bool on_pty_request(SshSession *session,
                            SshChannel *channel,
                            const std::string &term,
                            uint32_t width, uint32_t height,
                            uint32_t pixel_width, uint32_t pixel_height,
                            const std::vector<uint8_t> &modes) override
        {
            (void)session;
            (void)channel;
            (void)term;
            (void)width;
            (void)height;
            (void)pixel_width;
            (void)pixel_height;
            (void)modes;
            return true;
        }
    };

    SshSession session(2006, nullptr);
    session.set_state(SshSession::State::active);
    auto *channel = session.connection_manager().create_channel(
        SSH_CHANNEL_SESSION, 93, SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    TEST_ASSERT(channel != nullptr, "channel should be created for exec bridge opt-in test");

    ExecAcceptNoBridgeHandler handler;

    yuan::buffer::ByteBuffer pty_payload;
    SshMessageCodec::write_string(pty_payload, "xterm");
    SshMessageCodec::write_uint32(pty_payload, 100);
    SshMessageCodec::write_uint32(pty_payload, 30);
    SshMessageCodec::write_uint32(pty_payload, 1000);
    SshMessageCodec::write_uint32(pty_payload, 700);
    SshMessageCodec::write_string(pty_payload, std::string());
    auto pty_span = pty_payload.readable_span();
    SshChannelRequestMessage pty_req;
    pty_req.recipient_channel = 93;
    pty_req.request_type = "pty-req";
    pty_req.want_reply = true;
    pty_req.request_specific_data.assign(
        reinterpret_cast<const uint8_t *>(pty_span.data()),
        reinterpret_cast<const uint8_t *>(pty_span.data()) + pty_span.size());
    auto pty_packet = SshMessageCodec::encode_channel_request(pty_req);
    auto pty_packet_span = pty_packet.readable_span();
    std::vector<uint8_t> pty_raw(
        reinterpret_cast<const uint8_t *>(pty_packet_span.data()),
        reinterpret_cast<const uint8_t *>(pty_packet_span.data()) + pty_packet_span.size());
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_REQUEST, pty_raw, &handler);

    SshChannelRequestMessage exec_req;
    exec_req.recipient_channel = 93;
    exec_req.request_type = "exec";
    exec_req.want_reply = true;
    exec_req.request_specific_data = encode_string_payload("printf SHOULD_NOT_RUN");
    auto exec_packet = SshMessageCodec::encode_channel_request(exec_req);
    auto exec_packet_span = exec_packet.readable_span();
    std::vector<uint8_t> exec_raw(
        reinterpret_cast<const uint8_t *>(exec_packet_span.data()),
        reinterpret_cast<const uint8_t *>(exec_packet_span.data()) + exec_packet_span.size());
    session.dispatch(SshMessageType::SSH_MSG_CHANNEL_REQUEST, exec_raw, &handler);

    TEST_ASSERT(!session.has_pty_process(93),
                "exec PTY bridge should not start when handler does not opt in");
    return true;
#endif
}

int main()
{
    std::cout << "=== SSH Tests ===" << std::endl;

    RUN_TEST(test_password_auth_without_handler_uses_method_fallback);
    RUN_TEST(test_session_channel_open_without_handler_is_allowed);
    RUN_TEST(test_builtin_subsystem_request_without_handler_is_allowed);
    RUN_TEST(test_default_handler_denies_direct_tcpip_channel_open);
    RUN_TEST(test_default_handler_still_allows_builtin_subsystem);
    RUN_TEST(test_request_success_encoding_uses_message_byte);
    RUN_TEST(test_publickey_rsa_fallback_verifies_signature);
    RUN_TEST(test_publickey_ecdsa_fallback_verifies_signature);
    RUN_TEST(test_build_kex_init_uses_filtered_host_key_algorithms_in_config_order);
    RUN_TEST(test_process_kex_init_negotiates_config_preference_and_hash);
    RUN_TEST(test_first_kex_packet_follows_ignores_only_wrong_guess);
    RUN_TEST(test_process_newkeys_activates_encryption_after_kex);
    RUN_TEST(test_process_kex_init_message_uses_client_then_server_kex_payloads);
    RUN_TEST(test_reset_for_rekey_keeps_existing_encryption_active);
    RUN_TEST(test_rekey_preserves_session_id_across_new_exchange);
    RUN_TEST(test_local_file_system_basic_roundtrip);
    RUN_TEST(test_local_file_system_rejects_uid_gid_setstat);
    RUN_TEST(test_local_file_system_absolute_symlink_targets_stay_logical);
    RUN_TEST(test_local_file_system_realpath_and_readlink_edge_inputs);
    RUN_TEST(test_packet_codec_plaintext_roundtrip_and_partial_parse);
    RUN_TEST(test_try_parse_marks_invalid_for_encrypted_bytes_before_newkeys);
    RUN_TEST(test_packet_codec_encrypted_roundtrip_and_mac_failure);
    RUN_TEST(test_packet_codec_aead_roundtrip_and_tag_failure);
    RUN_TEST(test_channel_request_rejects_second_exec_and_second_shell);
    RUN_TEST(test_channel_request_rejects_second_pty_and_parses_modes_as_string);
    RUN_TEST(test_window_change_and_signal_always_reply_success_when_want_reply);
    RUN_TEST(test_direct_tcpip_open_requires_complete_type_specific_payload);
    RUN_TEST(test_direct_tcpip_open_valid_payload_is_still_handled_by_handler_policy);
    RUN_TEST(test_build_channel_exit_status_encodes_standard_channel_request);
    RUN_TEST(test_build_channel_exit_signal_encodes_standard_channel_request);
    RUN_TEST(test_session_dispatch_channel_close_emits_exit_status_before_close);
    RUN_TEST(test_session_dispatch_channel_close_without_command_sends_only_close);
    RUN_TEST(test_session_dispatch_channel_close_can_emit_exit_signal_via_handler);
    RUN_TEST(test_session_dispatch_repeated_channel_close_emits_exit_only_once);
    RUN_TEST(test_env_and_pty_rejected_after_command_started);
    RUN_TEST(test_shell_success_then_rejects_other_command_requests);
    RUN_TEST(test_channel_request_non_open_state_reply_behavior);
    RUN_TEST(test_subsystem_success_then_rejects_other_command_requests);
    RUN_TEST(test_session_dispatch_channel_close_uses_handler_exit_status_value);
    RUN_TEST(test_session_dispatch_channel_close_exit_signal_payload_fields);
    RUN_TEST(test_channel_request_sequence_matrix_command_then_exec_shell_subsystem_fail);
    RUN_TEST(test_terminal_session_state_records_accepted_pty_and_shell_flags);
    RUN_TEST(test_pty_backend_prepare_and_shutdown_lifecycle);
    RUN_TEST(test_pty_process_launch_shell_and_capture_output);
    RUN_TEST(test_session_pty_bridge_shell_data_and_exit_sequence);
    RUN_TEST(test_session_pty_bridge_window_change_affects_shell_stty_size);
    RUN_TEST(test_session_pty_bridge_signal_terminates_shell_child);
    RUN_TEST(test_session_pty_bridge_exec_with_pty_outputs_and_exits);
    RUN_TEST(test_session_pty_bridge_exec_requires_handler_opt_in);

    std::cout << std::endl;
    std::cout << "Tests run:    " << g_tests_run << std::endl;
    std::cout << "Tests passed: " << g_tests_passed << std::endl;
    std::cout << "Tests failed: " << g_tests_failed << std::endl;

    return g_tests_failed == 0 ? 0 : 1;
}
