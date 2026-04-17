#include "algorithm/ssh_cipher.h"
#include <openssl/evp.h>
#include <cstring>

namespace yuan::net::ssh
{
    class SshCipherAesCtr : public SshCipher
    {
    public:
        explicit SshCipherAesCtr(size_t key_len)
            : key_len_(key_len)
        {
        }

        ~SshCipherAesCtr() override
        {
            if (enc_ctx_)
                EVP_CIPHER_CTX_free(enc_ctx_);
            if (dec_ctx_)
                EVP_CIPHER_CTX_free(dec_ctx_);
        }

        SshCipherAesCtr(const SshCipherAesCtr &) = delete;
        SshCipherAesCtr &operator=(const SshCipherAesCtr &) = delete;

        std::string name() const override
        {
            if (key_len_ == 16)
                return "aes128-ctr";
            if (key_len_ == 24)
                return "aes192-ctr";
            return "aes256-ctr";
        }

        size_t block_size() const override
        {
            return 16;
        }

        size_t key_size() const override
        {
            return key_len_;
        }

        size_t iv_size() const override
        {
            return 16;
        }

        void init(const std::vector<uint8_t> &key,
                  const std::vector<uint8_t> &iv) override
        {
            key_ = key;
            iv_ = iv;

            const EVP_CIPHER *cipher = get_cipher();
            if (!cipher)
                return;

            enc_ctx_ = EVP_CIPHER_CTX_new();
            EVP_EncryptInit_ex(enc_ctx_, cipher, nullptr, key_.data(), iv_.data());

            dec_ctx_ = EVP_CIPHER_CTX_new();
            EVP_EncryptInit_ex(dec_ctx_, cipher, nullptr, key_.data(), iv_.data());
        }

        std::vector<uint8_t> encrypt(const uint8_t *data, size_t len) override
        {
            if (!enc_ctx_)
                return {};

            std::vector<uint8_t> out(len);
            int outlen = 0;
            EVP_EncryptUpdate(enc_ctx_, out.data(), &outlen, data, static_cast<int>(len));
            int tmplen = 0;
            EVP_EncryptFinal_ex(enc_ctx_, out.data() + outlen, &tmplen);
            out.resize(static_cast<size_t>(outlen + tmplen));
            return out;
        }

        std::vector<uint8_t> decrypt(const uint8_t *data, size_t len) override
        {
            if (!dec_ctx_)
                return {};

            std::vector<uint8_t> out(len);
            int outlen = 0;
            EVP_EncryptUpdate(dec_ctx_, out.data(), &outlen, data, static_cast<int>(len));
            int tmplen = 0;
            EVP_EncryptFinal_ex(dec_ctx_, out.data() + outlen, &tmplen);
            out.resize(static_cast<size_t>(outlen + tmplen));
            return out;
        }

    private:
        const EVP_CIPHER *get_cipher() const
        {
            if (key_len_ == 16)
                return EVP_aes_128_ctr();
            if (key_len_ == 24)
                return EVP_aes_192_ctr();
            if (key_len_ == 32)
                return EVP_aes_256_ctr();
            return nullptr;
        }

        size_t key_len_;
        std::vector<uint8_t> key_;
        std::vector<uint8_t> iv_;
        EVP_CIPHER_CTX *enc_ctx_ = nullptr;
        EVP_CIPHER_CTX *dec_ctx_ = nullptr;
    };

    std::unique_ptr<SshCipher> create_cipher_aes128_ctr()
    {
        return std::make_unique<SshCipherAesCtr>(16);
    }

    std::unique_ptr<SshCipher> create_cipher_aes192_ctr()
    {
        return std::make_unique<SshCipherAesCtr>(24);
    }

    std::unique_ptr<SshCipher> create_cipher_aes256_ctr()
    {
        return std::make_unique<SshCipherAesCtr>(32);
    }
}
