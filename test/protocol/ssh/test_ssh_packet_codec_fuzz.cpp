#include "ssh.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

using namespace yuan::net::ssh;

namespace
{
    class FakeCipher final : public SshCipher
    {
    public:
        std::string name() const override { return "test-cipher"; }
        size_t block_size() const override { return 8; }
        size_t key_size() const override { return 16; }
        size_t iv_size() const override { return 16; }
        void init(const std::vector<uint8_t> & key, const std::vector<uint8_t> & iv) override
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
        bool decrypt_length(const uint8_t * enc_length,
                            size_t enc_length_len,
                            const uint8_t *,
                            uint8_t * out_length) const override
        {
            if (enc_length_len < 4 || !out_length) {
                return false;
            }
            for (size_t i = 0; i < 4; ++i) {
                out_length[i] = enc_length[i];
            }
            return true;
        }

    private:
        bool initialized_ = false;
    };

    class FakeMac final : public SshMac
    {
    public:
        std::string name() const override { return "test-mac"; }
        size_t digest_size() const override { return 4; }
        size_t key_size() const override { return 16; }
        void init(const std::vector<uint8_t> & key) override { initialized_ = !key.empty(); }
        std::vector<uint8_t> compute(uint32_t, const uint8_t *, size_t) override
        {
            return { 0x01, 0x02, 0x03, 0x04 };
        }
        bool verify(uint32_t, const uint8_t *, size_t, const uint8_t * mac, size_t mac_len) override
        {
            return mac_len == 4 && mac[0] == 0x01 && mac[1] == 0x02 && mac[2] == 0x03 && mac[3] == 0x04;
        }

    private:
        bool initialized_ = false;
    };

    class FakeCompression final : public SshCompression
    {
    public:
        std::string name() const override { return "none"; }
        bool init() override { return true; }
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
        std::string name() const override { return "test-aead"; }
        size_t block_size() const override { return 8; }
        size_t key_size() const override { return 16; }
        size_t iv_size() const override { return 12; }
        size_t tag_size() const override { return 16; }
        void init(const std::vector<uint8_t> & key, const std::vector<uint8_t> & iv) override
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
        bool is_aead() const override { return true; }
        std::vector<uint8_t> encrypt_aead(const uint8_t * aad,
                                          size_t aad_len,
                                          const uint8_t * data,
                                          size_t data_len,
                                          const uint8_t *) override
        {
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
        bool decrypt_aead(const uint8_t * aad,
                          size_t aad_len,
                          const uint8_t * data,
                          size_t data_len,
                          const uint8_t * tag,
                          size_t tag_len,
                          const uint8_t *,
                          std::vector<uint8_t> & out) override
        {
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
        std::string name() const override { return "test-opaque"; }
        size_t block_size() const override { return 8; }
        size_t key_size() const override { return 16; }
        size_t iv_size() const override { return 16; }
        void init(const std::vector<uint8_t> & key, const std::vector<uint8_t> & iv) override
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
        bool decrypt_length(const uint8_t * enc_length,
                            size_t enc_length_len,
                            const uint8_t *,
                            uint8_t * out_length) const override
        {
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

    SshCipherContext build_active_cipher_context(SshAlgorithmRegistry & registry,
                                                 const std::string & cipher_name,
                                                 bool with_mac)
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
}

int main()
{
    const char * fuzz_env = std::getenv("YUAN_RUN_SSH_PACKET_CODEC_FUZZ");
    if (!fuzz_env || std::string(fuzz_env) != "1") {
        std::cout << "ssh_packet_codec_fuzz skipped (set YUAN_RUN_SSH_PACKET_CODEC_FUZZ=1)." << std::endl;
        return 0;
    }

    SshAlgorithmRegistry registry;
    registry.register_cipher("test-cipher", []() { return std::make_unique<FakeCipher>(); });
    registry.register_mac("test-mac", []() { return std::make_unique<FakeMac>(); });
    registry.register_cipher("test-aead", []() { return std::make_unique<FakeAeadCipher>(); });
    registry.register_cipher("test-opaque", []() { return std::make_unique<FakeOpaqueCipher>(); });
    registry.register_compression("none", []() { return std::make_unique<FakeCompression>(); });

    auto ctr_ctx = build_active_cipher_context(registry, "test-cipher", true);
    auto aead_ctx = build_active_cipher_context(registry, "test-aead", false);
    auto opaque_ctx = build_active_cipher_context(registry, "test-opaque", true);

    uint32_t seed = 0x1234ABCDu;
    auto next_u32 = [&seed]() -> uint32_t {
        seed = seed * 1664525u + 1013904223u;
        return seed;
    };

    constexpr int kIterations = 5000;
    for (int i = 0; i < kIterations; ++i) {
        const size_t len = static_cast<size_t>(next_u32() % 1024u);
        std::vector<uint8_t> raw(len);
        for (size_t j = 0; j < len; ++j) {
            raw[j] = static_cast<uint8_t>(next_u32() & 0xFFu);
        }

        yuan::buffer::ByteBuffer buf;
        if (!raw.empty()) {
            buf.append(raw.data(), raw.size());
        }

        const auto plain = SshPacketCodec::try_parse(buf, false, nullptr, static_cast<uint32_t>(i));
        if (plain.complete && plain.total_bytes > len) {
            std::cerr << "plain try_parse overflow at iter " << i << std::endl;
            return 1;
        }

        const auto ctr = SshPacketCodec::try_parse(buf, true, &ctr_ctx, static_cast<uint32_t>(i));
        if (ctr.complete && ctr.total_bytes > len) {
            std::cerr << "ctr try_parse overflow at iter " << i << std::endl;
            return 1;
        }

        const auto aead = SshPacketCodec::try_parse(buf, true, &aead_ctx, static_cast<uint32_t>(i));
        if (aead.complete && aead.total_bytes > len) {
            std::cerr << "aead try_parse overflow at iter " << i << std::endl;
            return 1;
        }

        const auto opaque = SshPacketCodec::try_parse(buf, true, &opaque_ctx, static_cast<uint32_t>(i));
        if (opaque.complete && opaque.total_bytes > len) {
            std::cerr << "opaque try_parse overflow at iter " << i << std::endl;
            return 1;
        }

        if (!raw.empty()) {
            (void)SshPacketCodec::decode(static_cast<uint32_t>(i), raw.data(), raw.size(), nullptr);
            (void)SshPacketCodec::decode(static_cast<uint32_t>(i), raw.data(), raw.size(), &ctr_ctx);
            (void)SshPacketCodec::decode(static_cast<uint32_t>(i), raw.data(), raw.size(), &aead_ctx);
            (void)SshPacketCodec::decode(static_cast<uint32_t>(i), raw.data(), raw.size(), &opaque_ctx);
        }
    }

    std::cout << "ssh_packet_codec_fuzz passed " << kIterations << " iterations." << std::endl;
    return 0;
}
