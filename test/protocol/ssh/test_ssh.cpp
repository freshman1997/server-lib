#include "ssh.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/x509.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
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
    RUN_TEST(test_packet_codec_plaintext_roundtrip_and_partial_parse);
    RUN_TEST(test_try_parse_marks_invalid_for_encrypted_bytes_before_newkeys);
    RUN_TEST(test_packet_codec_encrypted_roundtrip_and_mac_failure);
    RUN_TEST(test_packet_codec_aead_roundtrip_and_tag_failure);

    std::cout << std::endl;
    std::cout << "Tests run:    " << g_tests_run << std::endl;
    std::cout << "Tests passed: " << g_tests_passed << std::endl;
    std::cout << "Tests failed: " << g_tests_failed << std::endl;

    return g_tests_failed == 0 ? 0 : 1;
}
