#include "algorithm/ssh_kex_algorithm.h"
#include "crypto/ssh_crypto.h"
#include "crypto/ssh_key_derivation.h"
#include <cstring>

namespace yuan::net::ssh
{
    class SshKexCurve25519 : public SshKexAlgorithm
    {
    public:
        SshKexCurve25519() = default;

        std::string name() const override
        {
            return "curve25519-sha256";
        }

        size_t hash_digest_size() const override
        {
            return 32;
        }

        void set_crypto(SshCrypto *crypto) override
        {
            crypto_ = crypto;
        }

        std::vector<uint8_t> generate_public_key() override
        {
            if (!crypto_)
                return {};
            private_key_ = crypto_->curve25519_generate_key_pair(public_key_);
            return public_key_;
        }

        bool compute_shared_secret(const std::vector<uint8_t> &peer_public,
                                   std::vector<uint8_t> &shared_secret) override
        {
            if (!crypto_ || private_key_.empty())
                return false;
            shared_secret = crypto_->curve25519_compute_shared_secret(private_key_, peer_public);
            return !shared_secret.empty();
        }

        std::vector<uint8_t> compute_exchange_hash(
            const std::string &client_version,
            const std::string &server_version,
            const std::vector<uint8_t> &client_kex_init,
            const std::vector<uint8_t> &server_kex_init,
            const std::vector<uint8_t> &host_key,
            const std::vector<uint8_t> &client_public,
            const std::vector<uint8_t> &server_public,
            const std::vector<uint8_t> &shared_secret) override
        {
            return SshKeyDerivation::compute_exchange_hash_sha256(
                client_version, server_version,
                client_kex_init, server_kex_init,
                host_key, client_public, server_public,
                shared_secret);
        }

        std::vector<uint8_t> public_key() const override
        {
            return public_key_;
        }

    protected:
        SshCrypto *crypto_ = nullptr;
        std::vector<uint8_t> private_key_;
        std::vector<uint8_t> public_key_;
    };

    class SshKexCurve25519Libssh : public SshKexCurve25519
    {
    public:
        std::string name() const override
        {
            return "curve25519-sha256@libssh.org";
        }
    };

    std::unique_ptr<SshKexAlgorithm> create_kex_curve25519()
    {
        return std::make_unique<SshKexCurve25519>();
    }

    std::unique_ptr<SshKexAlgorithm> create_kex_curve25519_libssh()
    {
        return std::make_unique<SshKexCurve25519Libssh>();
    }
}
