#ifndef __NET_SSH_ALGORITHM_SSH_KEX_ALGORITHM_H__
#define __NET_SSH_ALGORITHM_SSH_KEX_ALGORITHM_H__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    class SshCrypto;

    class SshKexAlgorithm
    {
    public:
        virtual ~SshKexAlgorithm() = default;

        virtual std::string name() const = 0;

        virtual size_t hash_digest_size() const = 0;

        virtual std::vector<uint8_t> generate_public_key() = 0;

        virtual bool compute_shared_secret(const std::vector<uint8_t> &peer_public,
                                           std::vector<uint8_t> &shared_secret) = 0;

        virtual std::vector<uint8_t> compute_exchange_hash(
            const std::string &client_version,
            const std::string &server_version,
            const std::vector<uint8_t> &client_kex_init,
            const std::vector<uint8_t> &server_kex_init,
            const std::vector<uint8_t> &host_key,
            const std::vector<uint8_t> &client_public,
            const std::vector<uint8_t> &server_public,
            const std::vector<uint8_t> &shared_secret) = 0;

        virtual std::vector<uint8_t> public_key() const = 0;

        virtual void set_crypto(SshCrypto *crypto)
        {
            (void)crypto;
        }
    };
}

#endif
