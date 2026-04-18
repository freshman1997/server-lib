#include "algorithm/ssh_mac.h"
#include "openssl/hmac.h"
#include "openssl/evp.h"
#include <cstring>
#include <memory>

namespace yuan::net::ssh
{
    class SshMacHmacSha1 : public SshMac
    {
    public:
        SshMacHmacSha1() = default;

        std::string name() const override
        {
            return "hmac-sha1";
        }

        size_t digest_size() const override
        {
            return 20;
        }

        size_t key_size() const override
        {
            return 20;
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

            std::vector<uint8_t> mac_input;
            mac_input.reserve(4 + len);
            mac_input.insert(mac_input.end(), seq_bytes, seq_bytes + 4);
            mac_input.insert(mac_input.end(), data, data + len);

            std::vector<uint8_t> result(20);
            unsigned int outlen = 20;
            HMAC(EVP_sha1(), key_.data(), static_cast<int>(key_.size()),
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
        std::vector<uint8_t> key_;
    };

    std::unique_ptr<SshMac> create_mac_hmac_sha1()
    {
        return std::make_unique<SshMacHmacSha1>();
    }
}
