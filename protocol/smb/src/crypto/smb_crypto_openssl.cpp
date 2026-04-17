#include "crypto/smb_crypto_openssl.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/cmac.h>
#include <openssl/sha.h>
#include <cstring>

namespace yuan::net::smb
{
    SmbCryptoOpenSSL::SmbCryptoOpenSSL() = default;
    SmbCryptoOpenSSL::~SmbCryptoOpenSSL() = default;

    std::vector<uint8_t> SmbCryptoOpenSSL::aes128_cmac(const std::vector<uint8_t> & key, const uint8_t * data, size_t len)
    {
        std::vector<uint8_t> mac(16);
        size_t maclen = 16;
        CMAC_CTX *ctx = CMAC_CTX_new();
        CMAC_Init(ctx, key.data(), key.size(), EVP_aes_128_cbc(), nullptr);
        CMAC_Update(ctx, data, len);
        CMAC_Final(ctx, mac.data(), &maclen);
        CMAC_CTX_free(ctx);
        mac.resize(maclen);
        return mac;
    }

    std::vector<uint8_t> SmbCryptoOpenSSL::sign(const std::vector<uint8_t> & key, const uint8_t * data, size_t len)
    {
        return aes128_cmac(key, data, len);
    }

    bool SmbCryptoOpenSSL::verify(const std::vector<uint8_t> & key, const uint8_t * data, size_t len, const uint8_t * signature)
    {
        auto computed = aes128_cmac(key, data, len);
        if (computed.size() != 16)
            return false;
        return CRYPTO_memcmp(computed.data(), signature, 16) == 0;
    }

    std::vector<uint8_t> SmbCryptoOpenSSL::aes128_ccm_encrypt(const std::vector<uint8_t> & key,
                                                              const uint8_t * nonce, size_t nonce_len,
                                                              const uint8_t * aad, size_t aad_len,
                                                              const uint8_t * data, size_t data_len)
    {
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        std::vector<uint8_t> ciphertext(data_len + 16);
        int outlen = 0;
        int tmplen = 0;

        EVP_EncryptInit_ex(ctx, EVP_aes_128_ccm(), nullptr, nullptr, nullptr);

        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_IVLEN, static_cast<int>(nonce_len), nullptr);
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_TAG, 16, nullptr);

        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce);

        EVP_EncryptUpdate(ctx, nullptr, &outlen, nullptr, static_cast<int>(data_len));

        EVP_EncryptUpdate(ctx, nullptr, &outlen, aad, static_cast<int>(aad_len));

        EVP_EncryptUpdate(ctx, ciphertext.data(), &outlen, data, static_cast<int>(data_len));

        EVP_EncryptFinal_ex(ctx, ciphertext.data() + outlen, &tmplen);

        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_GET_TAG, 16, ciphertext.data() + data_len);

        EVP_CIPHER_CTX_free(ctx);

        ciphertext.resize(data_len + 16);
        return ciphertext;
    }

    bool SmbCryptoOpenSSL::aes128_ccm_decrypt(const std::vector<uint8_t> & key,
                                              const uint8_t * nonce, size_t nonce_len,
                                              const uint8_t * aad, size_t aad_len,
                                              const uint8_t * data, size_t data_len,
                                              uint8_t * out, size_t & out_len)
    {
        if (data_len < 16)
            return false;

        size_t payload_len = data_len - 16;
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        int outlen = 0;
        int tmplen = 0;
        bool ret = false;

        EVP_DecryptInit_ex(ctx, EVP_aes_128_ccm(), nullptr, nullptr, nullptr);

        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_IVLEN, static_cast<int>(nonce_len), nullptr);
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_TAG, 16, const_cast<uint8_t *>(data + payload_len));

        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce);

        EVP_DecryptUpdate(ctx, nullptr, &outlen, nullptr, static_cast<int>(payload_len));

        EVP_DecryptUpdate(ctx, nullptr, &outlen, aad, static_cast<int>(aad_len));

        if (EVP_DecryptUpdate(ctx, out, &outlen, data, static_cast<int>(payload_len)) > 0) {
            if (EVP_DecryptFinal_ex(ctx, out + outlen, &tmplen) > 0) {
                out_len = static_cast<size_t>(outlen + tmplen);
                ret = true;
            }
        }

        EVP_CIPHER_CTX_free(ctx);
        return ret;
    }

    std::vector<uint8_t> SmbCryptoOpenSSL::encrypt(const std::vector<uint8_t> & key,
                                                   const uint8_t * nonce, size_t nonce_len,
                                                   const uint8_t * aad, size_t aad_len,
                                                   const uint8_t * data, size_t data_len)
    {
        return aes128_ccm_encrypt(key, nonce, nonce_len, aad, aad_len, data, data_len);
    }

    bool SmbCryptoOpenSSL::decrypt(const std::vector<uint8_t> & key,
                                   const uint8_t * nonce, size_t nonce_len,
                                   const uint8_t * aad, size_t aad_len,
                                   const uint8_t * data, size_t data_len,
                                   uint8_t * out, size_t & out_len)
    {
        return aes128_ccm_decrypt(key, nonce, nonce_len, aad, aad_len, data, data_len, out, out_len);
    }

    std::vector<uint8_t> SmbCryptoOpenSSL::sha512(const uint8_t * data, size_t len)
    {
        std::vector<uint8_t> digest(SHA512_DIGEST_LENGTH);
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha512(), nullptr);
        EVP_DigestUpdate(ctx, data, len);
        EVP_DigestFinal_ex(ctx, digest.data(), nullptr);
        EVP_MD_CTX_free(ctx);
        return digest;
    }

    std::vector<uint8_t> SmbCryptoOpenSSL::hmac_sha256(const std::vector<uint8_t> & key, const uint8_t * data, size_t len)
    {
        std::vector<uint8_t> result(32);
        unsigned int outlen = 32;
        HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), data, len, result.data(), &outlen);
        return result;
    }
}
