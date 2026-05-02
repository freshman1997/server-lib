#ifndef __NET_SMB_CRYPTO_SMB_CRYPTO_OPENSSL_H__
#define __NET_SMB_CRYPTO_SMB_CRYPTO_OPENSSL_H__

#include "crypto/smb_crypto.h"
#include <memory>

namespace yuan::net::smb
{
    class SmbCryptoOpenSSL : public SmbCrypto
    {
    public:
        SmbCryptoOpenSSL();
        ~SmbCryptoOpenSSL() override;

        std::vector<uint8_t> sign(const std::vector<uint8_t> &key, const uint8_t *data, size_t len) override;
        bool verify(const std::vector<uint8_t> &key, const uint8_t *data, size_t len, const uint8_t *signature) override;
        std::vector<uint8_t> encrypt(const std::vector<uint8_t> &key, const uint8_t *nonce, size_t nonce_len,
                                     const uint8_t *aad, size_t aad_len,
                                     const uint8_t *data, size_t data_len) override;
        bool decrypt(const std::vector<uint8_t> &key, const uint8_t *nonce, size_t nonce_len,
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *data, size_t data_len,
                     uint8_t *out, size_t &out_len) override;
        std::vector<uint8_t> sha512(const uint8_t *data, size_t len) override;
        std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t> &key, const uint8_t *data, size_t len) override;
        std::vector<uint8_t> hmac_sha512(const std::vector<uint8_t> &key, const uint8_t *data, size_t len) override;

    private:
        std::vector<uint8_t> aes128_cmac(const std::vector<uint8_t> &key, const uint8_t *data, size_t len);
        std::vector<uint8_t> aes128_ccm_encrypt(const std::vector<uint8_t> &key, const uint8_t *nonce, size_t nonce_len,
                                                const uint8_t *aad, size_t aad_len,
                                                const uint8_t *data, size_t data_len);
        bool aes128_ccm_decrypt(const std::vector<uint8_t> &key, const uint8_t *nonce, size_t nonce_len,
                                const uint8_t *aad, size_t aad_len,
                                const uint8_t *data, size_t data_len,
                                uint8_t *out, size_t &out_len);
    };
}
#endif
