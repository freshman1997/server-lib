#ifndef __NET_SHADOWSOCKS_SHADOWSOCKS_CRYPTO_H__
#define __NET_SHADOWSOCKS_SHADOWSOCKS_CRYPTO_H__

#include "shadowsocks_protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::shadowsocks
{
    class ShadowsocksCrypto
    {
    public:
        static bool derive_master_key(const std::string &password,
                                      CipherMethod method,
                                      std::vector<uint8_t> &out_key);

        static bool derive_subkey(const std::vector<uint8_t> &master_key,
                                  CipherMethod method,
                                  const uint8_t *salt,
                                  std::size_t salt_size,
                                  std::vector<uint8_t> &out_subkey);

        static bool random_bytes(std::size_t size, std::vector<uint8_t> &out_bytes);

        static bool aead_encrypt(CipherMethod method,
                                 const std::vector<uint8_t> &key,
                                 const std::vector<uint8_t> &nonce,
                                 const uint8_t *plaintext,
                                 std::size_t plaintext_size,
                                 const uint8_t *aad,
                                 std::size_t aad_size,
                                 std::vector<uint8_t> &out_ciphertext_and_tag);

        static bool aead_decrypt(CipherMethod method,
                                 const std::vector<uint8_t> &key,
                                 const std::vector<uint8_t> &nonce,
                                 const uint8_t *ciphertext_and_tag,
                                 std::size_t ciphertext_and_tag_size,
                                 const uint8_t *aad,
                                 std::size_t aad_size,
                                 std::vector<uint8_t> &out_plaintext);

        static bool increment_nonce(std::vector<uint8_t> &nonce);
    };
}

#endif
