#ifndef __NET_SSH_CRYPTO_SSH_CRYPTO_H__
#define __NET_SSH_CRYPTO_SSH_CRYPTO_H__

#include "protocol/ssh_structures.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    class SshCrypto
    {
    public:
        virtual ~SshCrypto() = default;

        virtual std::vector<uint8_t> sha1(const uint8_t *data, size_t len) = 0;
        virtual std::vector<uint8_t> sha256(const uint8_t *data, size_t len) = 0;
        virtual std::vector<uint8_t> sha384(const uint8_t *data, size_t len) = 0;
        virtual std::vector<uint8_t> sha512(const uint8_t *data, size_t len) = 0;

        virtual std::vector<uint8_t> hmac_sha1(const std::vector<uint8_t> &key,
                                               const uint8_t *data, size_t len) = 0;
        virtual std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t> &key,
                                                 const uint8_t *data, size_t len) = 0;
        virtual std::vector<uint8_t> hmac_sha512(const std::vector<uint8_t> &key,
                                                 const uint8_t *data, size_t len) = 0;

        virtual std::vector<uint8_t> aes_ctr_encrypt(const std::vector<uint8_t> &key,
                                                     const std::vector<uint8_t> &iv,
                                                     const uint8_t *data, size_t len) = 0;
        virtual std::vector<uint8_t> aes_ctr_decrypt(const std::vector<uint8_t> &key,
                                                     const std::vector<uint8_t> &iv,
                                                     const uint8_t *data, size_t len) = 0;

        virtual std::vector<uint8_t> aes_gcm_encrypt(const std::vector<uint8_t> &key,
                                                     const std::vector<uint8_t> &iv,
                                                     const uint8_t *aad, size_t aad_len,
                                                     const uint8_t *data, size_t data_len) = 0;
        virtual bool aes_gcm_decrypt(const std::vector<uint8_t> &key,
                                     const std::vector<uint8_t> &iv,
                                     const uint8_t *aad, size_t aad_len,
                                     const uint8_t *data, size_t data_len,
                                     const uint8_t *tag, size_t tag_len,
                                     std::vector<uint8_t> &out) = 0;

        virtual std::vector<uint8_t> chacha20_poly1305_encrypt(const std::vector<uint8_t> &key,
                                                               const uint8_t *nonce,
                                                               const uint8_t *aad, size_t aad_len,
                                                               const uint8_t *data, size_t data_len) = 0;
        virtual bool chacha20_poly1305_decrypt(const std::vector<uint8_t> &key,
                                               const uint8_t *nonce,
                                               const uint8_t *aad, size_t aad_len,
                                               const uint8_t *data, size_t data_len,
                                               const uint8_t *tag, size_t tag_len,
                                               std::vector<uint8_t> &out) = 0;

        virtual std::vector<uint8_t> dh_generate_key_pair(const std::vector<uint8_t> &generator,
                                                          const std::vector<uint8_t> &prime,
                                                          std::vector<uint8_t> &public_key_out) = 0;
        virtual std::vector<uint8_t> dh_compute_shared_secret(const std::vector<uint8_t> &private_key,
                                                              const std::vector<uint8_t> &peer_public_key,
                                                              const std::vector<uint8_t> &prime) = 0;

        virtual std::vector<uint8_t> ecdh_generate_key_pair(const std::string &curve,
                                                            std::vector<uint8_t> &public_key_out) = 0;
        virtual std::vector<uint8_t> ecdh_compute_shared_secret(const std::string &curve,
                                                                const std::vector<uint8_t> &private_key,
                                                                const std::vector<uint8_t> &peer_public_key) = 0;

        virtual std::vector<uint8_t> curve25519_generate_key_pair(std::vector<uint8_t> &public_key_out) = 0;
        virtual std::vector<uint8_t> curve25519_compute_shared_secret(const std::vector<uint8_t> &private_key,
                                                                      const std::vector<uint8_t> &peer_public_key) = 0;

        virtual std::vector<uint8_t> rsa_sign(const std::vector<uint8_t> &private_key_der,
                                              const std::string &hash_alg,
                                              const uint8_t *data, size_t len) = 0;
        virtual bool rsa_verify(const std::vector<uint8_t> &public_key_der,
                                const std::string &hash_alg,
                                const uint8_t *data, size_t len,
                                const uint8_t *sig, size_t sig_len) = 0;

        virtual std::vector<uint8_t> ecdsa_sign(const std::vector<uint8_t> &private_key_der,
                                                const std::string &curve,
                                                const uint8_t *data, size_t len) = 0;
        virtual bool ecdsa_verify(const std::vector<uint8_t> &public_key_der,
                                  const std::string &curve,
                                  const uint8_t *data, size_t len,
                                  const uint8_t *sig, size_t sig_len) = 0;

        virtual std::vector<uint8_t> ed25519_sign(const std::vector<uint8_t> &private_key,
                                                  const uint8_t *data, size_t len) = 0;
        virtual bool ed25519_verify(const std::vector<uint8_t> &public_key,
                                    const uint8_t *data, size_t len,
                                    const uint8_t *sig, size_t sig_len) = 0;

        virtual SshKeyPair generate_rsa_key_pair(int bits) = 0;
        virtual SshKeyPair generate_ecdsa_key_pair(const std::string &curve) = 0;
        virtual SshKeyPair generate_ed25519_key_pair() = 0;

        virtual void random_bytes(uint8_t *out, size_t len) = 0;
    };
}

#endif
