#ifndef __NET_SSH_ALGORITHM_SSH_HOST_KEY_ALGORITHM_H__
#define __NET_SSH_ALGORITHM_SSH_HOST_KEY_ALGORITHM_H__

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    class SshCrypto;

    class SshHostKeyAlgorithm
    {
    public:
        virtual ~SshHostKeyAlgorithm() = default;

        virtual std::string name() const = 0;

        virtual std::vector<uint8_t> public_key_blob() const = 0;

        virtual std::vector<uint8_t> sign(const std::vector<uint8_t> &data) = 0;

        virtual bool verify(const std::vector<uint8_t> &data,
                            const std::vector<uint8_t> &signature) = 0;

        virtual std::string fingerprint() const = 0;

        virtual void set_crypto(SshCrypto *crypto)
        {
            (void)crypto;
        }

        virtual bool load_key_pair(const std::vector<uint8_t> &private_key_der,
                                   const std::vector<uint8_t> &public_key_der)
        {
            (void)private_key_der;
            (void)public_key_der;
            return false;
        }
    };
}

#endif
