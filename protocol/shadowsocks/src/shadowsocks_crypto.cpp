#include "shadowsocks_crypto.h"

#include <cstring>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

namespace yuan::net::shadowsocks
{
    namespace
    {
        const EVP_CIPHER *resolve_cipher(CipherMethod method)
        {
            switch (method) {
            case CipherMethod::aes_128_gcm:
                return EVP_aes_128_gcm();
            case CipherMethod::aes_256_gcm:
                return EVP_aes_256_gcm();
            case CipherMethod::chacha20_ietf_poly1305:
                return EVP_chacha20_poly1305();
            }
            return nullptr;
        }

        bool derive_like_evp_bytes_to_key_md5(const std::string &password,
                                              std::size_t key_size,
                                              std::vector<uint8_t> &out_key)
        {
            if (key_size == 0) {
                return false;
            }

            constexpr std::size_t kMd5Size = 16;
            std::vector<uint8_t> derived;
            derived.reserve(key_size + kMd5Size);

            std::vector<uint8_t> previous;
            previous.reserve(kMd5Size);

            while (derived.size() < key_size) {
                EVP_MD_CTX *ctx = EVP_MD_CTX_new();
                if (ctx == nullptr) {
                    out_key.clear();
                    return false;
                }

                if (EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) != 1) {
                    EVP_MD_CTX_free(ctx);
                    out_key.clear();
                    return false;
                }

                if (!previous.empty()) {
                    if (EVP_DigestUpdate(ctx, previous.data(), previous.size()) != 1) {
                        EVP_MD_CTX_free(ctx);
                        out_key.clear();
                        return false;
                    }
                }

                if (EVP_DigestUpdate(ctx, password.data(), password.size()) != 1) {
                    EVP_MD_CTX_free(ctx);
                    out_key.clear();
                    return false;
                }

                unsigned int out_len = 0;
                std::vector<uint8_t> digest(kMd5Size, 0);
                if (EVP_DigestFinal_ex(ctx, digest.data(), &out_len) != 1 || out_len != kMd5Size) {
                    EVP_MD_CTX_free(ctx);
                    out_key.clear();
                    return false;
                }
                EVP_MD_CTX_free(ctx);

                previous = digest;

                const std::size_t remain = key_size - derived.size();
                const std::size_t copy_size = remain < digest.size() ? remain : digest.size();
                derived.insert(derived.end(), digest.begin(), digest.begin() + static_cast<std::ptrdiff_t>(copy_size));
            }

            if (derived.size() != key_size) {
                out_key.clear();
                return false;
            }

            out_key = std::move(derived);
            return true;
        }

        bool hkdf_sha1(const uint8_t *ikm,
                       std::size_t ikm_size,
                       const uint8_t *salt,
                       std::size_t salt_size,
                       const uint8_t *info,
                       std::size_t info_size,
                       std::size_t out_size,
                       std::vector<uint8_t> &out)
        {
            out.clear();
            out.resize(out_size);

            EVP_KDF *kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
            if (kdf == nullptr) {
                out.clear();
                return false;
            }

            EVP_KDF_CTX *kdf_ctx = EVP_KDF_CTX_new(kdf);
            EVP_KDF_free(kdf);
            if (kdf_ctx == nullptr) {
                out.clear();
                return false;
            }

            OSSL_PARAM params[5];
            params[0] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
                                                         const_cast<char *>("SHA1"),
                                                         0);
            params[1] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
                                                          const_cast<uint8_t *>(ikm),
                                                          ikm_size);
            params[2] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                                          const_cast<uint8_t *>(salt),
                                                          salt_size);
            params[3] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO,
                                                          const_cast<uint8_t *>(info),
                                                          info_size);
            params[4] = OSSL_PARAM_construct_end();

            const int rc = EVP_KDF_derive(kdf_ctx, out.data(), out.size(), params);
            EVP_KDF_CTX_free(kdf_ctx);
            if (rc != 1) {
                out.clear();
                return false;
            }

            return true;
        }
    }

    bool ShadowsocksCrypto::derive_master_key(const std::string &password,
                                              CipherMethod method,
                                              std::vector<uint8_t> &out_key)
    {
        if (password.empty()) {
            out_key.clear();
            return false;
        }

        const auto &spec = method_spec(method);
        return derive_like_evp_bytes_to_key_md5(password, spec.key_size, out_key);
    }

    bool ShadowsocksCrypto::derive_subkey(const std::vector<uint8_t> &master_key,
                                          CipherMethod method,
                                          const uint8_t *salt,
                                          std::size_t salt_size,
                                          std::vector<uint8_t> &out_subkey)
    {
        const auto &spec = method_spec(method);
        if (master_key.size() != spec.key_size ||
            salt == nullptr ||
            salt_size != spec.salt_size) {
            out_subkey.clear();
            return false;
        }

        static constexpr uint8_t kInfo[] = "ss-subkey";
        return hkdf_sha1(master_key.data(), master_key.size(),
                         salt, salt_size,
                         kInfo, sizeof(kInfo) - 1,
                         spec.key_size,
                         out_subkey);
    }

    bool ShadowsocksCrypto::random_bytes(std::size_t size, std::vector<uint8_t> &out_bytes)
    {
        out_bytes.clear();
        out_bytes.resize(size);
        if (size == 0) {
            return true;
        }

        if (RAND_bytes(out_bytes.data(), static_cast<int>(size)) != 1) {
            out_bytes.clear();
            return false;
        }
        return true;
    }

    bool ShadowsocksCrypto::aead_encrypt(CipherMethod method,
                                         const std::vector<uint8_t> &key,
                                         const std::vector<uint8_t> &nonce,
                                         const uint8_t *plaintext,
                                         std::size_t plaintext_size,
                                         const uint8_t *aad,
                                         std::size_t aad_size,
                                         std::vector<uint8_t> &out_ciphertext_and_tag)
    {
        const auto &spec = method_spec(method);
        if (key.size() != spec.key_size || nonce.size() != spec.nonce_size) {
            out_ciphertext_and_tag.clear();
            return false;
        }

        const EVP_CIPHER *cipher = resolve_cipher(method);
        if (cipher == nullptr) {
            out_ciphertext_and_tag.clear();
            return false;
        }

        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        if (ctx == nullptr) {
            out_ciphertext_and_tag.clear();
            return false;
        }

        bool ok = false;
        int out_len = 0;
        int final_len = 0;
        std::vector<uint8_t> out(plaintext_size + spec.tag_size, 0);

        do {
            if (EVP_EncryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1) {
                break;
            }
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) != 1) {
                break;
            }
            if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
                break;
            }

            if (aad != nullptr && aad_size > 0) {
                if (EVP_EncryptUpdate(ctx, nullptr, &out_len, aad, static_cast<int>(aad_size)) != 1) {
                    break;
                }
            }

            if (plaintext_size > 0 && plaintext != nullptr) {
                if (EVP_EncryptUpdate(ctx, out.data(), &out_len,
                                      plaintext, static_cast<int>(plaintext_size)) != 1) {
                    break;
                }
            } else {
                out_len = 0;
            }

            if (EVP_EncryptFinal_ex(ctx, out.data() + out_len, &final_len) != 1) {
                break;
            }

            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG,
                                    static_cast<int>(spec.tag_size),
                                    out.data() + plaintext_size) != 1) {
                break;
            }

            ok = true;
        } while (false);

        EVP_CIPHER_CTX_free(ctx);

        if (!ok) {
            out_ciphertext_and_tag.clear();
            return false;
        }

        out.resize(plaintext_size + spec.tag_size);
        out_ciphertext_and_tag = std::move(out);
        return true;
    }

    bool ShadowsocksCrypto::aead_decrypt(CipherMethod method,
                                         const std::vector<uint8_t> &key,
                                         const std::vector<uint8_t> &nonce,
                                         const uint8_t *ciphertext_and_tag,
                                         std::size_t ciphertext_and_tag_size,
                                         const uint8_t *aad,
                                         std::size_t aad_size,
                                         std::vector<uint8_t> &out_plaintext)
    {
        const auto &spec = method_spec(method);
        if (key.size() != spec.key_size ||
            nonce.size() != spec.nonce_size ||
            ciphertext_and_tag == nullptr ||
            ciphertext_and_tag_size < spec.tag_size) {
            out_plaintext.clear();
            return false;
        }

        const EVP_CIPHER *cipher = resolve_cipher(method);
        if (cipher == nullptr) {
            out_plaintext.clear();
            return false;
        }

        const std::size_t ciphertext_size = ciphertext_and_tag_size - spec.tag_size;
        const uint8_t *tag = ciphertext_and_tag + ciphertext_size;

        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        if (ctx == nullptr) {
            out_plaintext.clear();
            return false;
        }

        bool ok = false;
        int out_len = 0;
        int final_len = 0;
        std::vector<uint8_t> out(ciphertext_size, 0);

        do {
            if (EVP_DecryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1) {
                break;
            }
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) != 1) {
                break;
            }
            if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
                break;
            }

            if (aad != nullptr && aad_size > 0) {
                if (EVP_DecryptUpdate(ctx, nullptr, &out_len, aad, static_cast<int>(aad_size)) != 1) {
                    break;
                }
            }

            if (ciphertext_size > 0) {
                if (EVP_DecryptUpdate(ctx, out.data(), &out_len,
                                      ciphertext_and_tag, static_cast<int>(ciphertext_size)) != 1) {
                    break;
                }
            } else {
                out_len = 0;
            }

            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG,
                                    static_cast<int>(spec.tag_size),
                                    const_cast<uint8_t *>(tag)) != 1) {
                break;
            }

            if (EVP_DecryptFinal_ex(ctx, out.data() + out_len, &final_len) != 1) {
                break;
            }

            ok = true;
        } while (false);

        EVP_CIPHER_CTX_free(ctx);

        if (!ok) {
            out_plaintext.clear();
            return false;
        }

        out.resize(ciphertext_size);
        out_plaintext = std::move(out);
        return true;
    }

    bool ShadowsocksCrypto::increment_nonce(std::vector<uint8_t> &nonce)
    {
        if (nonce.empty()) {
            return false;
        }

        for (std::size_t i = 0; i < nonce.size(); ++i) {
            ++nonce[i];
            if (nonce[i] != 0) {
                return true;
            }
        }

        return true;
    }
}
