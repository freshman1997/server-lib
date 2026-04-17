#ifndef __NET_SSH_ALGORITHM_SSH_CIPHER_H__
#define __NET_SSH_ALGORITHM_SSH_CIPHER_H__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    class SshCipher
    {
    public:
        virtual ~SshCipher() = default;

        virtual std::string name() const = 0;

        virtual size_t block_size() const = 0;

        virtual size_t key_size() const = 0;

        virtual size_t iv_size() const = 0;

        virtual size_t tag_size() const
        {
            return 0;
        }

        virtual void init(const std::vector<uint8_t> &key,
                          const std::vector<uint8_t> &iv) = 0;

        virtual std::vector<uint8_t> encrypt(const uint8_t *data, size_t len) = 0;

        virtual std::vector<uint8_t> decrypt(const uint8_t *data, size_t len) = 0;

        virtual bool is_aead() const
        {
            return false;
        }

        virtual std::vector<uint8_t> encrypt_aead(const uint8_t *aad, size_t aad_len,
                                                  const uint8_t *data, size_t data_len,
                                                  const uint8_t *seq_bytes)
        {
            return {};
        }

        virtual bool decrypt_aead(const uint8_t *aad, size_t aad_len,
                                  const uint8_t *data, size_t data_len,
                                  const uint8_t *tag, size_t tag_len,
                                  const uint8_t *seq_bytes,
                                  std::vector<uint8_t> &out)
        {
            return false;
        }

        virtual bool decrypt_length(const uint8_t *enc_length, size_t enc_length_len,
                                    const uint8_t *seq_bytes,
                                    uint8_t *out_length) const
        {
            (void)enc_length;
            (void)enc_length_len;
            (void)seq_bytes;
            (void)out_length;
            return false;
        }
    };
}

#endif
