#include "algorithm/ssh_cipher.h"
#include <openssl/evp.h>
#include <cstring>
#include <memory>

namespace yuan::net::ssh
{
    class SshCipherAesGcm : public SshCipher
    {
    public:
        explicit SshCipherAesGcm(size_t key_len)
            : key_len_(key_len)
        {
        }

        ~SshCipherAesGcm() override = default;

        std::string name() const override
        {
            if (key_len_ == 16)
                return "aes128-gcm@openssh.com";
            return "aes256-gcm@openssh.com";
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
            return 12;
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
            key_ = key;
            iv_ = iv;
            seq_counter_ = 0;
        }

        std::vector<uint8_t> encrypt(const uint8_t *data, size_t len) override
        {
            return encrypt_aead(nullptr, 0, data, len, nullptr);
        }

        std::vector<uint8_t> decrypt(const uint8_t *data, size_t len) override
        {
            std::vector<uint8_t> out;
            if (len < 16)
                return out;
            size_t ct_len = len - 16;
            if (!decrypt_aead(nullptr, 0, data, ct_len, data + ct_len, 16, nullptr, out))
                out.clear();
            return out;
        }

        std::vector<uint8_t> encrypt_aead(const uint8_t *aad, size_t aad_len,
                                          const uint8_t *data, size_t data_len,
                                          const uint8_t *seq_bytes) override
        {
            std::vector<uint8_t> nonce = compute_nonce(seq_bytes);

            const EVP_CIPHER *cipher = get_cipher();
            if (!cipher)
                return {};

            std::vector<uint8_t> out(data_len + 16);
            int outlen = 0;
            int tmplen = 0;

            EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
            EVP_EncryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr);
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
            EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_.data(), nonce.data());

            if (aad && aad_len > 0)
                EVP_EncryptUpdate(ctx, nullptr, &outlen, aad, static_cast<int>(aad_len));

            EVP_EncryptUpdate(ctx, out.data(), &outlen, data, static_cast<int>(data_len));
            EVP_EncryptFinal_ex(ctx, out.data() + outlen, &tmplen);

            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, out.data() + data_len);
            EVP_CIPHER_CTX_free(ctx);

            out.resize(data_len + 16);
            return out;
        }

        bool decrypt_aead(const uint8_t *aad, size_t aad_len,
                          const uint8_t *data, size_t data_len,
                          const uint8_t *tag, size_t tag_len,
                          const uint8_t *seq_bytes,
                          std::vector<uint8_t> &out) override
        {
            if (tag_len != 16)
                return false;

            std::vector<uint8_t> nonce = compute_nonce(seq_bytes);

            const EVP_CIPHER *cipher = get_cipher();
            if (!cipher)
                return false;

            out.resize(data_len);
            EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
            int outlen = 0;
            int tmplen = 0;
            bool ret = false;

            EVP_DecryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr);
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
            EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_.data(), nonce.data());

            if (aad && aad_len > 0)
                EVP_DecryptUpdate(ctx, nullptr, &outlen, aad, static_cast<int>(aad_len));

            EVP_DecryptUpdate(ctx, out.data(), &outlen, data, static_cast<int>(data_len));

            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag_len),
                                const_cast<uint8_t *>(tag));

            if (EVP_DecryptFinal_ex(ctx, out.data() + outlen, &tmplen) > 0) {
                out.resize(static_cast<size_t>(outlen + tmplen));
                ret = true;
            } else {
                out.clear();
            }

            EVP_CIPHER_CTX_free(ctx);
            return ret;
        }

    private:
        const EVP_CIPHER *get_cipher() const
        {
            if (key_len_ == 16)
                return EVP_aes_128_gcm();
            if (key_len_ == 32)
                return EVP_aes_256_gcm();
            return nullptr;
        }

        std::vector<uint8_t> compute_nonce(const uint8_t *seq_bytes) const
        {
            std::vector<uint8_t> nonce(12, 0);
            if (iv_.size() >= 12) {
                std::memcpy(nonce.data(), iv_.data(), 12);
            } else if (!iv_.empty()) {
                std::memcpy(nonce.data(), iv_.data(), iv_.size());
            }

            if (seq_bytes) {
                nonce[8] ^= seq_bytes[0];
                nonce[9] ^= seq_bytes[1];
                nonce[10] ^= seq_bytes[2];
                nonce[11] ^= seq_bytes[3];
            }

            return nonce;
        }

        size_t key_len_;
        std::vector<uint8_t> key_;
        std::vector<uint8_t> iv_;
        uint64_t seq_counter_ = 0;
    };

    std::unique_ptr<SshCipher> create_cipher_aes128_gcm()
    {
        return std::make_unique<SshCipherAesGcm>(16);
    }

    std::unique_ptr<SshCipher> create_cipher_aes256_gcm()
    {
        return std::make_unique<SshCipherAesGcm>(32);
    }
}
