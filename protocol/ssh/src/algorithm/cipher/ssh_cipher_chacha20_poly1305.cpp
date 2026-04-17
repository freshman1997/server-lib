#include "algorithm/ssh_cipher.h"
#include <openssl/evp.h>
#include <cstring>

namespace yuan::net::ssh
{
    class SshCipherChacha20Poly1305 : public SshCipher
    {
    public:
        SshCipherChacha20Poly1305() = default;

        ~SshCipherChacha20Poly1305() override
        {
            if (enc_ctx_)
                EVP_CIPHER_CTX_free(enc_ctx_);
            if (dec_ctx_)
                EVP_CIPHER_CTX_free(dec_ctx_);
        }

        SshCipherChacha20Poly1305(const SshCipherChacha20Poly1305 &) = delete;
        SshCipherChacha20Poly1305 &operator=(const SshCipherChacha20Poly1305 &) = delete;

        std::string name() const override
        {
            return "chacha20-poly1305@openssh.com";
        }

        size_t block_size() const override
        {
            return 8;
        }

        size_t key_size() const override
        {
            return 64;
        }

        size_t iv_size() const override
        {
            return 0;
        }

        size_t tag_size() const override
        {
            return 16;
        }

        bool is_aead() const override
        {
            return true;
        }

        void init(const std::vector<uint8_t> &key,
                  const std::vector<uint8_t> &iv) override
        {
            (void)iv;
            if (key.size() < 64)
                return;

            k2_.assign(key.begin(), key.begin() + 32);
            k1_.assign(key.begin() + 32, key.end());
        }

        std::vector<uint8_t> encrypt(const uint8_t *data, size_t len) override
        {
            return encrypt_aead(nullptr, 0, data, len, nullptr);
        }

        std::vector<uint8_t> decrypt(const uint8_t *data, size_t len) override
        {
            std::vector<uint8_t> out;
            if (len < 4 + 16)
                return out;
            size_t ct_len = len - 4 - 16;
            if (!decrypt_aead(nullptr, 0, data, len - 16, data + len - 16, 16, nullptr, out))
                out.clear();
            return out;
        }

        std::vector<uint8_t> encrypt_aead(const uint8_t *aad, size_t aad_len,
                                          const uint8_t *data, size_t data_len,
                                          const uint8_t *seq_bytes) override
        {
            uint8_t nonce[12];
            build_nonce(nonce, seq_bytes);

            uint8_t enc_length[4];
            encrypt_length_impl(aad, aad_len, nonce, enc_length);

            EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
            EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr);
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr);
            EVP_EncryptInit_ex(ctx, nullptr, nullptr, k1_.data(), nonce);

            int outlen = 0;
            EVP_EncryptUpdate(ctx, nullptr, &outlen, enc_length, 4);

            std::vector<uint8_t> out(data_len + 16);
            EVP_EncryptUpdate(ctx, out.data(), &outlen, data, static_cast<int>(data_len));
            int tmplen = 0;
            EVP_EncryptFinal_ex(ctx, out.data() + outlen, &tmplen);
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, out.data() + data_len);
            EVP_CIPHER_CTX_free(ctx);

            std::vector<uint8_t> result;
            result.reserve(4 + data_len + 16);
            result.insert(result.end(), enc_length, enc_length + 4);
            result.insert(result.end(), out.begin(), out.begin() + data_len + 16);
            return result;
        }

        bool decrypt_aead(const uint8_t *aad, size_t aad_len,
                          const uint8_t *data, size_t data_len,
                          const uint8_t *tag, size_t tag_len,
                          const uint8_t *seq_bytes,
                          std::vector<uint8_t> &out) override
        {
            (void)aad;
            (void)aad_len;

            if (data_len < 4 || tag_len != 16)
                return false;

            uint8_t nonce[12];
            build_nonce(nonce, seq_bytes);

            size_t ciphertext_len = data_len - 4;
            const uint8_t *ciphertext = data + 4;

            EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
            EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr);
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr);
            EVP_DecryptInit_ex(ctx, nullptr, nullptr, k1_.data(), nonce);

            int outlen = 0;
            EVP_DecryptUpdate(ctx, nullptr, &outlen, data, 4);

            out.resize(ciphertext_len);
            EVP_DecryptUpdate(ctx, out.data(), &outlen, ciphertext, static_cast<int>(ciphertext_len));

            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, static_cast<int>(tag_len),
                                const_cast<uint8_t *>(tag));

            int tmplen = 0;
            bool ok = EVP_DecryptFinal_ex(ctx, out.data() + outlen, &tmplen) > 0;
            EVP_CIPHER_CTX_free(ctx);

            if (!ok) {
                out.clear();
                return false;
            }

            out.resize(static_cast<size_t>(outlen + tmplen));
            return true;
        }

        bool decrypt_length(const uint8_t *enc_length, size_t enc_length_len,
                            const uint8_t *seq_bytes,
                            uint8_t *out_length) const override
        {
            if (enc_length_len < 4 || k2_.empty())
                return false;

            uint8_t nonce[12];
            build_nonce(nonce, seq_bytes);

            EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
            EVP_EncryptInit_ex(ctx, EVP_chacha20(), nullptr, k2_.data(), nonce);
            int outlen = 0;
            EVP_EncryptUpdate(ctx, out_length, &outlen, enc_length, static_cast<int>(enc_length_len));
            int tmplen = 0;
            EVP_EncryptFinal_ex(ctx, out_length + outlen, &tmplen);
            EVP_CIPHER_CTX_free(ctx);
            return true;
        }

    private:
        void build_nonce(uint8_t nonce[12], const uint8_t *seq_bytes) const
        {
            std::memset(nonce, 0, 12);
            nonce[8] = seq_bytes[3];
            nonce[9] = seq_bytes[2];
            nonce[10] = seq_bytes[1];
            nonce[11] = seq_bytes[0];
        }

        void encrypt_length_impl(const uint8_t *plain_len, size_t plain_len_size,
                                 const uint8_t *nonce, uint8_t *out) const
        {
            EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
            EVP_EncryptInit_ex(ctx, EVP_chacha20(), nullptr, k2_.data(), nonce);
            int outlen = 0;
            EVP_EncryptUpdate(ctx, out, &outlen, plain_len, static_cast<int>(plain_len_size));
            int tmplen = 0;
            EVP_EncryptFinal_ex(ctx, out + outlen, &tmplen);
            EVP_CIPHER_CTX_free(ctx);
        }

        std::vector<uint8_t> k2_;
        std::vector<uint8_t> k1_;
        EVP_CIPHER_CTX *enc_ctx_ = nullptr;
        EVP_CIPHER_CTX *dec_ctx_ = nullptr;
    };

    std::unique_ptr<SshCipher> create_cipher_chacha20_poly1305()
    {
        return std::make_unique<SshCipherChacha20Poly1305>();
    }
}
