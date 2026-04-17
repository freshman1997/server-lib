#ifndef __NET_SMB_CRYPTO_SMB_CRYPTO_H__
#define __NET_SMB_CRYPTO_SMB_CRYPTO_H__

#include <cstdint>
#include <vector>

namespace yuan::net::smb
{
    class SmbCrypto
    {
    public:
        virtual ~SmbCrypto() = default;
        virtual std::vector<uint8_t> sign(const std::vector<uint8_t> &key, const uint8_t *data, size_t len) = 0;
        virtual bool verify(const std::vector<uint8_t> &key, const uint8_t *data, size_t len, const uint8_t *signature) = 0;
        virtual std::vector<uint8_t> encrypt(const std::vector<uint8_t> &key, const uint8_t *nonce, size_t nonce_len,
                                             const uint8_t *aad, size_t aad_len,
                                             const uint8_t *data, size_t data_len) = 0;
        virtual bool decrypt(const std::vector<uint8_t> &key, const uint8_t *nonce, size_t nonce_len,
                             const uint8_t *aad, size_t aad_len,
                             const uint8_t *data, size_t data_len,
                             uint8_t *out, size_t &out_len) = 0;
        virtual std::vector<uint8_t> sha512(const uint8_t *data, size_t len) = 0;
        virtual std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t> &key, const uint8_t *data, size_t len) = 0;
    };
}
#endif
