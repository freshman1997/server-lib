#ifndef __NET_SSH_CRYPTO_SSH_KEY_DERIVATION_H__
#define __NET_SSH_CRYPTO_SSH_KEY_DERIVATION_H__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    class SshKeyDerivation
    {
    public:
        static std::vector<uint8_t> derive_key(const std::vector<uint8_t> &K,
                                               const std::vector<uint8_t> &H,
                                               const std::vector<uint8_t> &session_id,
                                               char letter,
                                               size_t key_len,
                                               const std::string &hash_name = "sha256");

        static std::vector<uint8_t> compute_exchange_hash_sha256(
            const std::string &client_version,
            const std::string &server_version,
            const std::vector<uint8_t> &client_kex_init,
            const std::vector<uint8_t> &server_kex_init,
            const std::vector<uint8_t> &host_key,
            const std::vector<uint8_t> &client_public,
            const std::vector<uint8_t> &server_public,
            const std::vector<uint8_t> &shared_secret);

        static std::vector<uint8_t> compute_exchange_hash_sha384(
            const std::string &client_version,
            const std::string &server_version,
            const std::vector<uint8_t> &client_kex_init,
            const std::vector<uint8_t> &server_kex_init,
            const std::vector<uint8_t> &host_key,
            const std::vector<uint8_t> &client_public,
            const std::vector<uint8_t> &server_public,
            const std::vector<uint8_t> &shared_secret);

        static std::vector<uint8_t> compute_exchange_hash_sha512(
            const std::string &client_version,
            const std::string &server_version,
            const std::vector<uint8_t> &client_kex_init,
            const std::vector<uint8_t> &server_kex_init,
            const std::vector<uint8_t> &host_key,
            const std::vector<uint8_t> &client_public,
            const std::vector<uint8_t> &server_public,
            const std::vector<uint8_t> &shared_secret);

        static std::vector<uint8_t> derive_session_id(const std::vector<uint8_t> &exchange_hash);
    };
}

#endif
