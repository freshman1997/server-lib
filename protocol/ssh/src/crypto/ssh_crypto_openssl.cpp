#include "crypto/ssh_crypto_openssl.h"
#include "openssl/evp.h"
#include "openssl/hmac.h"
#include "openssl/sha.h"
#include "openssl/rand.h"
#include "openssl/dh.h"
#include "openssl/ec.h"
#include "openssl/rsa.h"
#include "openssl/pem.h"
#include "openssl/bio.h"
#include "openssl/err.h"
#include "openssl/core_names.h"
#include "openssl/param_build.h"
#include "openssl/params.h"
#include <cstring>
#include <stdexcept>

namespace yuan::net::ssh
{
    SshCryptoOpenSSL::SshCryptoOpenSSL() = default;
    SshCryptoOpenSSL::~SshCryptoOpenSSL() = default;

    std::vector<uint8_t> SshCryptoOpenSSL::sha1(const uint8_t * data, size_t len)
    {
        std::vector<uint8_t> digest(SHA_DIGEST_LENGTH);
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
        EVP_DigestUpdate(ctx, data, len);
        EVP_DigestFinal_ex(ctx, digest.data(), nullptr);
        EVP_MD_CTX_free(ctx);
        return digest;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::sha256(const uint8_t * data, size_t len)
    {
        std::vector<uint8_t> digest(SHA256_DIGEST_LENGTH);
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
        EVP_DigestUpdate(ctx, data, len);
        EVP_DigestFinal_ex(ctx, digest.data(), nullptr);
        EVP_MD_CTX_free(ctx);
        return digest;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::sha384(const uint8_t * data, size_t len)
    {
        std::vector<uint8_t> digest(SHA384_DIGEST_LENGTH);
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha384(), nullptr);
        EVP_DigestUpdate(ctx, data, len);
        EVP_DigestFinal_ex(ctx, digest.data(), nullptr);
        EVP_MD_CTX_free(ctx);
        return digest;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::sha512(const uint8_t * data, size_t len)
    {
        std::vector<uint8_t> digest(SHA512_DIGEST_LENGTH);
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha512(), nullptr);
        EVP_DigestUpdate(ctx, data, len);
        EVP_DigestFinal_ex(ctx, digest.data(), nullptr);
        EVP_MD_CTX_free(ctx);
        return digest;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::hmac_sha1(const std::vector<uint8_t> & key,
                                                     const uint8_t * data, size_t len)
    {
        std::vector<uint8_t> result(SHA_DIGEST_LENGTH);
        unsigned int outlen = SHA_DIGEST_LENGTH;
        HMAC(EVP_sha1(), key.data(), static_cast<int>(key.size()), data, len, result.data(), &outlen);
        return result;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::hmac_sha256(const std::vector<uint8_t> & key,
                                                       const uint8_t * data, size_t len)
    {
        std::vector<uint8_t> result(SHA256_DIGEST_LENGTH);
        unsigned int outlen = SHA256_DIGEST_LENGTH;
        HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), data, len, result.data(), &outlen);
        return result;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::hmac_sha512(const std::vector<uint8_t> & key,
                                                       const uint8_t * data, size_t len)
    {
        std::vector<uint8_t> result(SHA512_DIGEST_LENGTH);
        unsigned int outlen = SHA512_DIGEST_LENGTH;
        HMAC(EVP_sha512(), key.data(), static_cast<int>(key.size()), data, len, result.data(), &outlen);
        return result;
    }

    static const EVP_CIPHER *get_aes_ctr_cipher(size_t key_len)
    {
        if (key_len == 16)
            return EVP_aes_128_ctr();
        if (key_len == 24)
            return EVP_aes_192_ctr();
        if (key_len == 32)
            return EVP_aes_256_ctr();
        return nullptr;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::aes_ctr_encrypt(const std::vector<uint8_t> & key,
                                                           const std::vector<uint8_t> & iv,
                                                           const uint8_t * data, size_t len)
    {
        const EVP_CIPHER *cipher = get_aes_ctr_cipher(key.size());
        if (!cipher)
            return {};

        std::vector<uint8_t> out(len);
        int outlen = 0;
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        EVP_EncryptInit_ex(ctx, cipher, nullptr, key.data(), iv.data());
        EVP_EncryptUpdate(ctx, out.data(), &outlen, data, static_cast<int>(len));
        int tmplen = 0;
        EVP_EncryptFinal_ex(ctx, out.data() + outlen, &tmplen);
        EVP_CIPHER_CTX_free(ctx);
        out.resize(static_cast<size_t>(outlen + tmplen));
        return out;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::aes_ctr_decrypt(const std::vector<uint8_t> & key,
                                                           const std::vector<uint8_t> & iv,
                                                           const uint8_t * data, size_t len)
    {
        const EVP_CIPHER *cipher = get_aes_ctr_cipher(key.size());
        if (!cipher)
            return {};

        std::vector<uint8_t> out(len);
        int outlen = 0;
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        EVP_DecryptInit_ex(ctx, cipher, nullptr, key.data(), iv.data());
        EVP_DecryptUpdate(ctx, out.data(), &outlen, data, static_cast<int>(len));
        int tmplen = 0;
        EVP_DecryptFinal_ex(ctx, out.data() + outlen, &tmplen);
        EVP_CIPHER_CTX_free(ctx);
        out.resize(static_cast<size_t>(outlen + tmplen));
        return out;
    }

    static const EVP_CIPHER *get_aes_gcm_cipher(size_t key_len)
    {
        if (key_len == 16)
            return EVP_aes_128_gcm();
        if (key_len == 24)
            return EVP_aes_192_gcm();
        if (key_len == 32)
            return EVP_aes_256_gcm();
        return nullptr;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::aes_gcm_encrypt(const std::vector<uint8_t> & key,
                                                           const std::vector<uint8_t> & iv,
                                                           const uint8_t * aad, size_t aad_len,
                                                           const uint8_t * data, size_t data_len)
    {
        const EVP_CIPHER *cipher = get_aes_gcm_cipher(key.size());
        if (!cipher)
            return {};

        std::vector<uint8_t> out(data_len + 16);
        int outlen = 0;
        int tmplen = 0;
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();

        EVP_EncryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr);
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr);
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data());

        if (aad && aad_len > 0)
            EVP_EncryptUpdate(ctx, nullptr, &outlen, aad, static_cast<int>(aad_len));

        EVP_EncryptUpdate(ctx, out.data(), &outlen, data, static_cast<int>(data_len));
        EVP_EncryptFinal_ex(ctx, out.data() + outlen, &tmplen);

        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, out.data() + data_len);
        EVP_CIPHER_CTX_free(ctx);

        out.resize(data_len + 16);
        return out;
    }

    bool SshCryptoOpenSSL::aes_gcm_decrypt(const std::vector<uint8_t> & key,
                                           const std::vector<uint8_t> & iv,
                                           const uint8_t * aad, size_t aad_len,
                                           const uint8_t * data, size_t data_len,
                                           const uint8_t * tag, size_t tag_len,
                                           std::vector<uint8_t> & out)
    {
        const EVP_CIPHER *cipher = get_aes_gcm_cipher(key.size());
        if (!cipher)
            return false;
        if (tag_len != 16)
            return false;

        out.resize(data_len);
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        int outlen = 0;
        int tmplen = 0;
        bool ret = false;

        EVP_DecryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr);
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr);
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data());

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

    std::vector<uint8_t> SshCryptoOpenSSL::chacha20_poly1305_encrypt(
        const std::vector<uint8_t> & key,
        const uint8_t * nonce,
        const uint8_t * aad, size_t aad_len,
        const uint8_t * data, size_t data_len)
    {
        std::vector<uint8_t> out(data_len + 16);
        int outlen = 0;
        int tmplen = 0;
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();

        EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr);
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr);
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce);

        if (aad && aad_len > 0)
            EVP_EncryptUpdate(ctx, nullptr, &outlen, aad, static_cast<int>(aad_len));

        EVP_EncryptUpdate(ctx, out.data(), &outlen, data, static_cast<int>(data_len));
        EVP_EncryptFinal_ex(ctx, out.data() + outlen, &tmplen);

        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, out.data() + data_len);
        EVP_CIPHER_CTX_free(ctx);

        out.resize(data_len + 16);
        return out;
    }

    bool SshCryptoOpenSSL::chacha20_poly1305_decrypt(
        const std::vector<uint8_t> & key,
        const uint8_t * nonce,
        const uint8_t * aad, size_t aad_len,
        const uint8_t * data, size_t data_len,
        const uint8_t * tag, size_t tag_len,
        std::vector<uint8_t> & out)
    {
        if (tag_len != 16)
            return false;

        out.resize(data_len);
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        int outlen = 0;
        int tmplen = 0;
        bool ret = false;

        EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr);
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr);
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce);

        if (aad && aad_len > 0)
            EVP_DecryptUpdate(ctx, nullptr, &outlen, aad, static_cast<int>(aad_len));

        EVP_DecryptUpdate(ctx, out.data(), &outlen, data, static_cast<int>(data_len));

        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, static_cast<int>(tag_len),
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

    std::vector<uint8_t> SshCryptoOpenSSL::dh_generate_key_pair(
        const std::vector<uint8_t> & generator,
        const std::vector<uint8_t> & prime,
        std::vector<uint8_t> & public_key_out)
    {
        OSSL_PARAM params[3];
        auto prime_copy = prime;
        auto generator_copy = generator;
        params[0] = OSSL_PARAM_construct_BN(OSSL_PKEY_PARAM_FFC_P, prime_copy.data(), prime_copy.size());
        params[1] = OSSL_PARAM_construct_BN(OSSL_PKEY_PARAM_FFC_G, generator_copy.data(), generator_copy.size());
        params[2] = OSSL_PARAM_construct_end();

        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(nullptr, "DH", nullptr);
        EVP_PKEY *pkey = nullptr;
        EVP_PKEY_CTX_set_params(ctx, params);
        EVP_PKEY_keygen_init(ctx);
        EVP_PKEY_generate(ctx, &pkey);

        BIGNUM *pub_key = nullptr;
        BIGNUM *priv_key = nullptr;
        EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_PUB_KEY, &pub_key);
        EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_PRIV_KEY, &priv_key);

        public_key_out.resize(BN_num_bytes(pub_key));
        BN_bn2bin(pub_key, public_key_out.data());

        std::vector<uint8_t> private_key(BN_num_bytes(priv_key));
        BN_bn2bin(priv_key, private_key.data());

        BN_free(pub_key);
        BN_free(priv_key);
        EVP_PKEY_free(pkey);
        EVP_PKEY_CTX_free(ctx);

        return private_key;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::dh_compute_shared_secret(
        const std::vector<uint8_t> & private_key,
        const std::vector<uint8_t> & peer_public_key,
        const std::vector<uint8_t> & prime)
    {
        BIGNUM *p = BN_bin2bn(prime.data(), static_cast<int>(prime.size()), nullptr);
        BIGNUM *g = BN_new();
        BN_set_word(g, 2);
        BIGNUM *priv = BN_bin2bn(private_key.data(), static_cast<int>(private_key.size()), nullptr);
        BIGNUM *peer_pub = BN_bin2bn(peer_public_key.data(), static_cast<int>(peer_public_key.size()), nullptr);

        BIGNUM *shared = BN_new();
        BN_CTX *bn_ctx = BN_CTX_new();
        BN_mod_exp(shared, peer_pub, priv, p, bn_ctx);

        std::vector<uint8_t> result(BN_num_bytes(shared));
        BN_bn2bin(shared, result.data());

        BN_free(shared);
        BN_CTX_free(bn_ctx);
        BN_free(peer_pub);
        BN_free(priv);
        BN_free(g);
        BN_free(p);

        return result;
    }

    static int get_ec_nid(const std::string & curve)
    {
        if (curve == "nistp256" || curve == "P-256" || curve == "prime256v1")
            return NID_X9_62_prime256v1;
        if (curve == "nistp384" || curve == "P-384" || curve == "secp384r1")
            return NID_secp384r1;
        if (curve == "nistp521" || curve == "P-521" || curve == "secp521r1")
            return NID_secp521r1;
        return NID_undef;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::ecdh_generate_key_pair(
        const std::string & curve,
        std::vector<uint8_t> & public_key_out)
    {
        int nid = get_ec_nid(curve);
        if (nid == NID_undef)
            return {};

        EVP_PKEY *pkey = nullptr;
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        OSSL_PARAM params[3];
        const char *group_name = (curve == "nistp256" || curve == "P-256" || curve == "prime256v1") ? "prime256v1"
                                                                                                    : (curve == "nistp384" || curve == "P-384" || curve == "secp384r1") ? "secp384r1"
                                                                                                                                                                        : "secp521r1";
        params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
                                                     const_cast<char *>(group_name), 0);
        params[1] = OSSL_PARAM_construct_end();

        EVP_PKEY_keygen_init(ctx);
        EVP_PKEY_CTX_set_params(ctx, params);
        EVP_PKEY_generate(ctx, &pkey);

        BIGNUM *pub_x = nullptr;
        BIGNUM *pub_y = nullptr;
        BIGNUM *priv = nullptr;
        EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_EC_PUB_X, &pub_x);
        EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_EC_PUB_Y, &pub_y);
        EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_PRIV_KEY, &priv);

        size_t coord_len = (nid == NID_X9_62_prime256v1) ? 32
                                                         : (nid == NID_secp384r1) ? 48
                                                                                  : 66;

        public_key_out.resize(1 + 2 * coord_len);
        public_key_out[0] = 0x04;
        BN_bn2binpad(pub_x, public_key_out.data() + 1, static_cast<int>(coord_len));
        BN_bn2binpad(pub_y, public_key_out.data() + 1 + coord_len, static_cast<int>(coord_len));

        std::vector<uint8_t> private_key(BN_num_bytes(priv));
        BN_bn2bin(priv, private_key.data());

        BN_free(pub_x);
        BN_free(pub_y);
        BN_free(priv);
        EVP_PKEY_free(pkey);
        EVP_PKEY_CTX_free(ctx);

        return private_key;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::ecdh_compute_shared_secret(
        const std::string & curve,
        const std::vector<uint8_t> & private_key,
        const std::vector<uint8_t> & peer_public_key)
    {
        int nid = get_ec_nid(curve);
        if (nid == NID_undef)
            return {};

        size_t coord_len = (nid == NID_X9_62_prime256v1) ? 32
                                                         : (nid == NID_secp384r1) ? 48
                                                                                  : 66;

        if (peer_public_key.size() < 1 + 2 * coord_len)
            return {};

        const char *group_name = (nid == NID_X9_62_prime256v1) ? "prime256v1"
                                                               : (nid == NID_secp384r1) ? "secp384r1"
                                                                                        : "secp521r1";

        OSSL_PARAM params[4];
        params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
                                                     const_cast<char *>(group_name), 0);
        std::vector<uint8_t> pub_x(peer_public_key.begin() + 1, peer_public_key.begin() + 1 + coord_len);
        std::vector<uint8_t> pub_y(peer_public_key.begin() + 1 + coord_len, peer_public_key.begin() + 1 + 2 * coord_len);
        auto private_key_copy = private_key;
        params[1] = OSSL_PARAM_construct_BN(OSSL_PKEY_PARAM_EC_PUB_X, pub_x.data(), pub_x.size());
        params[2] = OSSL_PARAM_construct_BN(OSSL_PKEY_PARAM_EC_PUB_Y, pub_y.data(), pub_y.size());
        params[3] = OSSL_PARAM_construct_end();

        EVP_PKEY_CTX *pkey_ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        EVP_PKEY *peer_pkey = nullptr;
        EVP_PKEY_fromdata_init(pkey_ctx);
        EVP_PKEY_fromdata(pkey_ctx, &peer_pkey, EVP_PKEY_PUBLIC_KEY, params);

        OSSL_PARAM priv_params[3];
        priv_params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
                                                          const_cast<char *>(group_name), 0);
        priv_params[1] = OSSL_PARAM_construct_BN(OSSL_PKEY_PARAM_PRIV_KEY,
                                                 private_key_copy.data(), private_key_copy.size());
        priv_params[2] = OSSL_PARAM_construct_end();

        EVP_PKEY_CTX *priv_ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        EVP_PKEY *our_pkey = nullptr;
        EVP_PKEY_fromdata_init(priv_ctx);
        EVP_PKEY_fromdata(priv_ctx, &our_pkey, EVP_PKEY_KEYPAIR, priv_params);

        EVP_PKEY_CTX *derive_ctx = EVP_PKEY_CTX_new(our_pkey, nullptr);
        EVP_PKEY_derive_init(derive_ctx);
        EVP_PKEY_derive_set_peer(derive_ctx, peer_pkey);

        size_t secret_len = 0;
        EVP_PKEY_derive(derive_ctx, nullptr, &secret_len);

        std::vector<uint8_t> shared_secret(secret_len);
        EVP_PKEY_derive(derive_ctx, shared_secret.data(), &secret_len);

        EVP_PKEY_CTX_free(derive_ctx);
        EVP_PKEY_free(our_pkey);
        EVP_PKEY_CTX_free(priv_ctx);
        EVP_PKEY_free(peer_pkey);
        EVP_PKEY_CTX_free(pkey_ctx);

        return shared_secret;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::curve25519_generate_key_pair(
        std::vector<uint8_t> & public_key_out)
    {
        EVP_PKEY *pkey = nullptr;
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(nullptr, "X25519", nullptr);
        EVP_PKEY_keygen_init(ctx);
        EVP_PKEY_generate(ctx, &pkey);

        size_t pub_len = 32;
        public_key_out.resize(pub_len);
        EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PUB_KEY,
                                        public_key_out.data(), pub_len, &pub_len);

        size_t priv_len = 32;
        std::vector<uint8_t> private_key(priv_len);
        EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PRIV_KEY,
                                        private_key.data(), priv_len, &priv_len);

        EVP_PKEY_free(pkey);
        EVP_PKEY_CTX_free(ctx);
        return private_key;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::curve25519_compute_shared_secret(
        const std::vector<uint8_t> & private_key,
        const std::vector<uint8_t> & peer_public_key)
    {
        OSSL_PARAM priv_params[2];
        priv_params[0] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PRIV_KEY,
                                                           const_cast<uint8_t *>(private_key.data()),
                                                           private_key.size());
        priv_params[1] = OSSL_PARAM_construct_end();

        EVP_PKEY_CTX *priv_ctx = EVP_PKEY_CTX_new_from_name(nullptr, "X25519", nullptr);
        EVP_PKEY *our_pkey = nullptr;
        EVP_PKEY_fromdata_init(priv_ctx);
        EVP_PKEY_fromdata(priv_ctx, &our_pkey, EVP_PKEY_KEYPAIR, priv_params);

        OSSL_PARAM pub_params[2];
        pub_params[0] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY,
                                                          const_cast<uint8_t *>(peer_public_key.data()),
                                                          peer_public_key.size());
        pub_params[1] = OSSL_PARAM_construct_end();

        EVP_PKEY_CTX *pub_ctx = EVP_PKEY_CTX_new_from_name(nullptr, "X25519", nullptr);
        EVP_PKEY *peer_pkey = nullptr;
        EVP_PKEY_fromdata_init(pub_ctx);
        EVP_PKEY_fromdata(pub_ctx, &peer_pkey, EVP_PKEY_PUBLIC_KEY, pub_params);

        EVP_PKEY_CTX *derive_ctx = EVP_PKEY_CTX_new(our_pkey, nullptr);
        EVP_PKEY_derive_init(derive_ctx);
        EVP_PKEY_derive_set_peer(derive_ctx, peer_pkey);

        size_t secret_len = 0;
        EVP_PKEY_derive(derive_ctx, nullptr, &secret_len);

        std::vector<uint8_t> shared_secret(secret_len);
        EVP_PKEY_derive(derive_ctx, shared_secret.data(), &secret_len);

        EVP_PKEY_CTX_free(derive_ctx);
        EVP_PKEY_free(peer_pkey);
        EVP_PKEY_CTX_free(pub_ctx);
        EVP_PKEY_free(our_pkey);
        EVP_PKEY_CTX_free(priv_ctx);

        return shared_secret;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::rsa_sign(
        const std::vector<uint8_t> & private_key_der,
        const std::string & hash_alg,
        const uint8_t * data, size_t len)
    {
        const uint8_t *der_ptr = private_key_der.data();
        EVP_PKEY *pkey = d2i_AutoPrivateKey(nullptr, &der_ptr, static_cast<long>(private_key_der.size()));
        if (!pkey)
            return {};

        const EVP_MD *md = nullptr;
        if (hash_alg == "sha256")
            md = EVP_sha256();
        else if (hash_alg == "sha512")
            md = EVP_sha512();
        else if (hash_alg == "sha1")
            md = EVP_sha1();
        else {
            EVP_PKEY_free(pkey);
            return {};
        }

        EVP_PKEY_CTX *sign_ctx = nullptr;
        EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(md_ctx, &sign_ctx, md, nullptr, pkey);

        size_t req_len = 0;
        EVP_DigestSign(md_ctx, nullptr, &req_len, data, len);
        std::vector<uint8_t> sig(req_len);
        EVP_DigestSign(md_ctx, sig.data(), &req_len, data, len);
        sig.resize(req_len);

        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        return sig;
    }

    bool SshCryptoOpenSSL::rsa_verify(
        const std::vector<uint8_t> & public_key_der,
        const std::string & hash_alg,
        const uint8_t * data, size_t len,
        const uint8_t * sig, size_t sig_len)
    {
        const uint8_t *der_ptr = public_key_der.data();
        EVP_PKEY *pkey = d2i_PUBKEY(nullptr, &der_ptr, static_cast<long>(public_key_der.size()));
        if (!pkey)
            return false;

        const EVP_MD *md = nullptr;
        if (hash_alg == "sha256")
            md = EVP_sha256();
        else if (hash_alg == "sha512")
            md = EVP_sha512();
        else if (hash_alg == "sha1")
            md = EVP_sha1();
        else {
            EVP_PKEY_free(pkey);
            return false;
        }

        EVP_PKEY_CTX *verify_ctx = nullptr;
        EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
        EVP_DigestVerifyInit(md_ctx, &verify_ctx, md, nullptr, pkey);
        int result = EVP_DigestVerify(md_ctx, sig, sig_len, data, len);

        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        return result == 1;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::ecdsa_sign(
        const std::vector<uint8_t> & private_key_der,
        const std::string & curve,
        const uint8_t * data, size_t len)
    {
        const uint8_t *der_ptr = private_key_der.data();
        EVP_PKEY *pkey = d2i_AutoPrivateKey(nullptr, &der_ptr, static_cast<long>(private_key_der.size()));
        if (!pkey)
            return {};

        int nid = get_ec_nid(curve);
        const EVP_MD *md = (nid == NID_X9_62_prime256v1) ? EVP_sha256()
                                                         : (nid == NID_secp384r1) ? EVP_sha384()
                                                                                  : EVP_sha512();

        EVP_PKEY_CTX *sign_ctx = nullptr;
        EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(md_ctx, &sign_ctx, md, nullptr, pkey);

        size_t req_len = 0;
        EVP_DigestSign(md_ctx, nullptr, &req_len, data, len);
        std::vector<uint8_t> sig(req_len);
        EVP_DigestSign(md_ctx, sig.data(), &req_len, data, len);
        sig.resize(req_len);

        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        return sig;
    }

    bool SshCryptoOpenSSL::ecdsa_verify(
        const std::vector<uint8_t> & public_key_der,
        const std::string & curve,
        const uint8_t * data, size_t len,
        const uint8_t * sig, size_t sig_len)
    {
        const uint8_t *der_ptr = public_key_der.data();
        EVP_PKEY *pkey = d2i_PUBKEY(nullptr, &der_ptr, static_cast<long>(public_key_der.size()));
        if (!pkey)
            return false;

        int nid = get_ec_nid(curve);
        const EVP_MD *md = (nid == NID_X9_62_prime256v1) ? EVP_sha256()
                                                         : (nid == NID_secp384r1) ? EVP_sha384()
                                                                                  : EVP_sha512();

        EVP_PKEY_CTX *verify_ctx = nullptr;
        EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
        EVP_DigestVerifyInit(md_ctx, &verify_ctx, md, nullptr, pkey);
        int result = EVP_DigestVerify(md_ctx, sig, sig_len, data, len);

        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        return result == 1;
    }

    std::vector<uint8_t> SshCryptoOpenSSL::ed25519_sign(
        const std::vector<uint8_t> & private_key,
        const uint8_t * data, size_t len)
    {
        OSSL_PARAM params[2];
        params[0] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PRIV_KEY,
                                                      const_cast<uint8_t *>(private_key.data()),
                                                      private_key.size());
        params[1] = OSSL_PARAM_construct_end();

        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(nullptr, "ED25519", nullptr);
        EVP_PKEY *pkey = nullptr;
        EVP_PKEY_fromdata_init(ctx);
        EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_KEYPAIR, params);

        size_t sig_len = 64;
        std::vector<uint8_t> sig(sig_len);
        EVP_PKEY_CTX *sign_ctx = nullptr;
        EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(md_ctx, &sign_ctx, nullptr, nullptr, pkey);
        EVP_DigestSign(md_ctx, sig.data(), &sig_len, data, len);

        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        EVP_PKEY_CTX_free(ctx);
        sig.resize(sig_len);
        return sig;
    }

    bool SshCryptoOpenSSL::ed25519_verify(
        const std::vector<uint8_t> & public_key,
        const uint8_t * data, size_t len,
        const uint8_t * sig, size_t sig_len)
    {
        if (sig_len != 64)
            return false;

        OSSL_PARAM params[2];
        params[0] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY,
                                                      const_cast<uint8_t *>(public_key.data()),
                                                      public_key.size());
        params[1] = OSSL_PARAM_construct_end();

        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(nullptr, "ED25519", nullptr);
        EVP_PKEY *pkey = nullptr;
        EVP_PKEY_fromdata_init(ctx);
        EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params);

        EVP_PKEY_CTX *verify_ctx = nullptr;
        EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
        EVP_DigestVerifyInit(md_ctx, &verify_ctx, nullptr, nullptr, pkey);
        int result = EVP_DigestVerify(md_ctx, sig, sig_len, data, len);

        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        EVP_PKEY_CTX_free(ctx);
        return result == 1;
    }

    SshKeyPair SshCryptoOpenSSL::generate_rsa_key_pair(int bits)
    {
        EVP_PKEY *pkey = nullptr;
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
        EVP_PKEY_keygen_init(ctx);
        OSSL_PARAM params[2];
        size_t bits_value = static_cast<size_t>(bits);
        params[0] = OSSL_PARAM_construct_size_t(OSSL_PKEY_PARAM_BITS, &bits_value);
        params[1] = OSSL_PARAM_construct_end();
        EVP_PKEY_CTX_set_params(ctx, params);
        EVP_PKEY_generate(ctx, &pkey);

        SshKeyPair keypair;

        uint8_t *priv_der = nullptr;
        int priv_len = i2d_PrivateKey(pkey, &priv_der);
        keypair.private_key.assign(priv_der, priv_der + priv_len);
        OPENSSL_free(priv_der);

        uint8_t *pub_der = nullptr;
        int pub_len = i2d_PUBKEY(pkey, &pub_der);
        keypair.public_key.assign(pub_der, pub_der + pub_len);
        OPENSSL_free(pub_der);

        EVP_PKEY_free(pkey);
        EVP_PKEY_CTX_free(ctx);
        return keypair;
    }

    SshKeyPair SshCryptoOpenSSL::generate_ecdsa_key_pair(const std::string & curve)
    {
        int nid = get_ec_nid(curve);
        if (nid == NID_undef)
            return {};

        const char *group_name = (nid == NID_X9_62_prime256v1) ? "prime256v1"
                                                               : (nid == NID_secp384r1) ? "secp384r1"
                                                                                        : "secp521r1";

        EVP_PKEY *pkey = nullptr;
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        OSSL_PARAM params[2];
        params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
                                                     const_cast<char *>(group_name), 0);
        params[1] = OSSL_PARAM_construct_end();
        EVP_PKEY_keygen_init(ctx);
        EVP_PKEY_CTX_set_params(ctx, params);
        EVP_PKEY_generate(ctx, &pkey);

        SshKeyPair keypair;

        uint8_t *priv_der = nullptr;
        int priv_len = i2d_PrivateKey(pkey, &priv_der);
        keypair.private_key.assign(priv_der, priv_der + priv_len);
        OPENSSL_free(priv_der);

        uint8_t *pub_der = nullptr;
        int pub_len = i2d_PUBKEY(pkey, &pub_der);
        keypair.public_key.assign(pub_der, pub_der + pub_len);
        OPENSSL_free(pub_der);

        EVP_PKEY_free(pkey);
        EVP_PKEY_CTX_free(ctx);
        return keypair;
    }

    SshKeyPair SshCryptoOpenSSL::generate_ed25519_key_pair()
    {
        EVP_PKEY *pkey = nullptr;
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(nullptr, "ED25519", nullptr);
        EVP_PKEY_keygen_init(ctx);
        EVP_PKEY_generate(ctx, &pkey);

        SshKeyPair keypair;

        size_t pub_len = 32;
        keypair.public_key.resize(pub_len);
        EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PUB_KEY,
                                        keypair.public_key.data(), pub_len, &pub_len);

        size_t priv_len = 32;
        keypair.private_key.resize(priv_len);
        EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PRIV_KEY,
                                        keypair.private_key.data(), priv_len, &priv_len);

        EVP_PKEY_free(pkey);
        EVP_PKEY_CTX_free(ctx);
        return keypair;
    }

    void SshCryptoOpenSSL::random_bytes(uint8_t * out, size_t len)
    {
        RAND_bytes(out, static_cast<int>(len));
    }
}
