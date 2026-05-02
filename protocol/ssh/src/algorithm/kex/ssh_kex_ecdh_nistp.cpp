#include "algorithm/ssh_kex_algorithm.h"
#include "crypto/ssh_crypto.h"
#include "crypto/ssh_key_derivation.h"
#include <cstring>
#include <memory>

namespace yuan::net::ssh
{
    class SshKexEcdhNistp256 : public SshKexAlgorithm
    {
    public:
        std::string name() const override
        {
            return "ecdh-sha2-nistp256";
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
            private_key_ = crypto_->ecdh_generate_key_pair("P-256", public_key_);
            return public_key_;
        }

        bool compute_shared_secret(const std::vector<uint8_t> &peer_public,
                                   std::vector<uint8_t> &shared_secret) override
        {
            if (!crypto_ || private_key_.empty())
                return false;
            shared_secret = crypto_->ecdh_compute_shared_secret("P-256", private_key_, peer_public);
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
            return SshKeyDerivation::compute_ecdh_exchange_hash_sha256(
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

    class SshKexEcdhNistp384 : public SshKexAlgorithm
    {
    public:
        std::string name() const override
        {
            return "ecdh-sha2-nistp384";
        }
        size_t hash_digest_size() const override
        {
            return 48;
        }

        void set_crypto(SshCrypto *crypto) override
        {
            crypto_ = crypto;
        }

        std::vector<uint8_t> generate_public_key() override
        {
            if (!crypto_)
                return {};
            private_key_ = crypto_->ecdh_generate_key_pair("P-384", public_key_);
            return public_key_;
        }

        bool compute_shared_secret(const std::vector<uint8_t> &peer_public,
                                   std::vector<uint8_t> &shared_secret) override
        {
            if (!crypto_ || private_key_.empty())
                return false;
            shared_secret = crypto_->ecdh_compute_shared_secret("P-384", private_key_, peer_public);
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
            return SshKeyDerivation::compute_ecdh_exchange_hash_sha384(
                client_version, server_version,
                client_kex_init, server_kex_init,
                host_key, client_public, server_public,
                shared_secret);
        }

        std::vector<uint8_t> public_key() const override
        {
            return public_key_;
        }

    private:
        SshCrypto *crypto_ = nullptr;
        std::vector<uint8_t> private_key_;
        std::vector<uint8_t> public_key_;
    };

    class SshKexEcdhNistp521 : public SshKexAlgorithm
    {
    public:
        std::string name() const override
        {
            return "ecdh-sha2-nistp521";
        }
        size_t hash_digest_size() const override
        {
            return 64;
        }

        void set_crypto(SshCrypto *crypto) override
        {
            crypto_ = crypto;
        }

        std::vector<uint8_t> generate_public_key() override
        {
            if (!crypto_)
                return {};
            private_key_ = crypto_->ecdh_generate_key_pair("P-521", public_key_);
            return public_key_;
        }

        bool compute_shared_secret(const std::vector<uint8_t> &peer_public,
                                   std::vector<uint8_t> &shared_secret) override
        {
            if (!crypto_ || private_key_.empty())
                return false;
            shared_secret = crypto_->ecdh_compute_shared_secret("P-521", private_key_, peer_public);
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
            return SshKeyDerivation::compute_ecdh_exchange_hash_sha512(
                client_version, server_version,
                client_kex_init, server_kex_init,
                host_key, client_public, server_public,
                shared_secret);
        }

        std::vector<uint8_t> public_key() const override
        {
            return public_key_;
        }

    private:
        SshCrypto *crypto_ = nullptr;
        std::vector<uint8_t> private_key_;
        std::vector<uint8_t> public_key_;
    };

    std::unique_ptr<SshKexAlgorithm> create_kex_ecdh_nistp256()
    {
        return std::make_unique<SshKexEcdhNistp256>();
    }

    std::unique_ptr<SshKexAlgorithm> create_kex_ecdh_nistp384()
    {
        return std::make_unique<SshKexEcdhNistp384>();
    }

    std::unique_ptr<SshKexAlgorithm> create_kex_ecdh_nistp521()
    {
        return std::make_unique<SshKexEcdhNistp521>();
    }
}
