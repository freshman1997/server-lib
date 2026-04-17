#include "algorithm/ssh_mac.h"
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <cstring>

namespace yuan::net::ssh
{
    class SshMacHmacSha2 : public SshMac
    {
    public:
        explicit SshMacHmacSha2(size_t digest_size)
            : digest_size_(digest_size)
        {
        }

        std::string name() const override
        {
            if (digest_size_ == 64)
                return "hmac-sha2-512";
            return "hmac-sha2-256";
        }

        size_t digest_size() const override
        {
            return digest_size_;
        }

        size_t key_size() const override
        {
            return digest_size_;
        }

        void init(const std::vector<uint8_t> &key) override
        {
            key_ = key;
        }

        std::vector<uint8_t> compute(uint32_t seq,
                                     const uint8_t *data, size_t len) override
        {
            uint8_t seq_bytes[4];
            seq_bytes[0] = static_cast<uint8_t>((seq >> 24) & 0xFF);
            seq_bytes[1] = static_cast<uint8_t>((seq >> 16) & 0xFF);
            seq_bytes[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
            seq_bytes[3] = static_cast<uint8_t>(seq & 0xFF);

            const EVP_MD *md = (digest_size_ == 64) ? EVP_sha512() : EVP_sha256();

            std::vector<uint8_t> mac_input;
            mac_input.reserve(4 + len);
            mac_input.insert(mac_input.end(), seq_bytes, seq_bytes + 4);
            mac_input.insert(mac_input.end(), data, data + len);

            std::vector<uint8_t> result(digest_size_);
            unsigned int outlen = static_cast<unsigned int>(digest_size_);
            HMAC(md, key_.data(), static_cast<int>(key_.size()),
                 mac_input.data(), mac_input.size(),
                 result.data(), &outlen);
            return result;
        }

        bool verify(uint32_t seq,
                    const uint8_t *data, size_t len,
                    const uint8_t *mac, size_t mac_len) override
        {
            auto computed = compute(seq, data, len);
            if (computed.size() != mac_len)
                return false;
            return CRYPTO_memcmp(computed.data(), mac, mac_len) == 0;
        }

    private:
        size_t digest_size_;
        std::vector<uint8_t> key_;
    };

    std::unique_ptr<SshMac> create_mac_hmac_sha2_256()
    {
        return std::make_unique<SshMacHmacSha2>(32);
    }

    std::unique_ptr<SshMac> create_mac_hmac_sha2_512()
    {
        return std::make_unique<SshMacHmacSha2>(64);
    }
}
