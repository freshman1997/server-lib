#include "hostkey/ssh_host_key_provider.h"
#include "crypto/ssh_crypto.h"
#include "crypto/ssh_crypto_openssl.h"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <cstring>
#include <fstream>

namespace yuan::net::ssh
{
    namespace
    {
        std::string key_type_to_algorithm_name(SshHostKeyType type)
        {
            switch (type) {
            case SshHostKeyType::ED25519:
                return "ssh-ed25519";
            case SshHostKeyType::ECDSA_P256:
                return "ecdsa-sha2-nistp256";
            case SshHostKeyType::ECDSA_P384:
                return "ecdsa-sha2-nistp384";
            case SshHostKeyType::ECDSA_P521:
                return "ecdsa-sha2-nistp521";
            case SshHostKeyType::RSA:
                return "rsa-sha2-512";
            default:
                return "";
            }
        }

        std::unique_ptr<SshHostKeyAlgorithm> create_algorithm_by_type(SshHostKeyType type)
        {
            switch (type) {
            case SshHostKeyType::ED25519:
                return create_host_key_ed25519();
            case SshHostKeyType::ECDSA_P256:
                return create_host_key_ecdsa_nistp256();
            case SshHostKeyType::ECDSA_P384:
                return create_host_key_ecdsa_nistp384();
            case SshHostKeyType::ECDSA_P521:
                return create_host_key_ecdsa_nistp521();
            case SshHostKeyType::RSA:
                return create_host_key_rsa_sha512();
            default:
                return nullptr;
            }
        }

        bool read_file_bytes(const std::string &path, std::string &out)
        {
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs.is_open())
                return false;
            out.assign(std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>());
            return true;
        }
    }

    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_ed25519();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_rsa_sha512();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_rsa_sha256();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_rsa();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_ecdsa_nistp256();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_ecdsa_nistp384();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_ecdsa_nistp521();

    bool SshHostKeyProvider::load_key(const std::string & path, SshHostKeyType type)
    {
        std::string file_data;
        if (!read_file_bytes(path, file_data))
            return false;

        BIO *bio = BIO_new_mem_buf(file_data.data(), static_cast<int>(file_data.size()));
        EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);

        if (!pkey)
            return false;

        std::vector<uint8_t> priv_der;
        uint8_t *der_ptr = nullptr;
        int der_len = i2d_PrivateKey(pkey, &der_ptr);
        if (der_len > 0 && der_ptr) {
            priv_der.assign(der_ptr, der_ptr + der_len);
            OPENSSL_free(der_ptr);
        }

        std::vector<uint8_t> pub_der;
        uint8_t *pub_ptr = nullptr;
        int pub_len = i2d_PUBKEY(pkey, &pub_ptr);
        if (pub_len > 0 && pub_ptr) {
            pub_der.assign(pub_ptr, pub_ptr + pub_len);
            OPENSSL_free(pub_ptr);
        }

        EVP_PKEY_free(pkey);

        auto algo = create_algorithm_by_type(type);
        if (!algo)
            return false;

        auto crypto = std::make_unique<SshCryptoOpenSSL>();
        algo->set_crypto(crypto.get());

        if (!algo->load_key_pair(priv_der, pub_der))
            return false;

        KeyEntry entry;
        entry.type = type;
        entry.algorithm_name = key_type_to_algorithm_name(type);
        entry.crypto = std::move(crypto);
        entry.algorithm = std::move(algo);
        entries_.push_back(std::move(entry));

        return true;
    }

    bool SshHostKeyProvider::load_or_generate(const std::string & path, SshHostKeyType type)
    {
        if (load_key(path, type))
            return true;

        if (!generate_key(type, path))
            return false;

        return load_key(path, type);
    }

    SshHostKeyAlgorithm *SshHostKeyProvider::find_algorithm(const std::string & algo_name) const
    {
        for (const auto &entry : entries_) {
            if (entry.algorithm_name == algo_name)
                return entry.algorithm.get();

            if (algo_name == "ssh-rsa" && entry.type == SshHostKeyType::RSA)
                return entry.algorithm.get();

            if ((algo_name == "rsa-sha2-256" || algo_name == "rsa-sha2-512") &&
                entry.type == SshHostKeyType::RSA)
                return entry.algorithm.get();
        }
        return nullptr;
    }

    std::vector<std::string> SshHostKeyProvider::supported_algorithm_names() const
    {
        std::vector<std::string> names;
        names.reserve(entries_.size());
        for (const auto &entry : entries_)
            names.push_back(entry.algorithm_name);
        return names;
    }

    SshHostKeyAlgorithm *SshHostKeyProvider::default_algorithm() const
    {
        if (entries_.empty())
            return nullptr;
        return entries_.front().algorithm.get();
    }

    bool SshHostKeyProvider::generate_key(SshHostKeyType type, const std::string & path)
    {
        SshCryptoOpenSSL crypto;
        SshKeyPair keypair;

        switch (type) {
        case SshHostKeyType::ED25519:
            keypair = crypto.generate_ed25519_key_pair();
            break;
        case SshHostKeyType::ECDSA_P256:
            keypair = crypto.generate_ecdsa_key_pair("P-256");
            break;
        case SshHostKeyType::ECDSA_P384:
            keypair = crypto.generate_ecdsa_key_pair("P-384");
            break;
        case SshHostKeyType::ECDSA_P521:
            keypair = crypto.generate_ecdsa_key_pair("P-521");
            break;
        case SshHostKeyType::RSA:
            keypair = crypto.generate_rsa_key_pair(3072);
            break;
        default:
            return false;
        }

        if (keypair.private_key.empty())
            return false;

        const uint8_t *p = keypair.private_key.data();
        EVP_PKEY *pkey = d2i_AutoPrivateKey(nullptr, &p, static_cast<long>(keypair.private_key.size()));
        if (!pkey)
            return false;

        BIO *bio = BIO_new_file(path.c_str(), "w");
        if (!bio) {
            EVP_PKEY_free(pkey);
            return false;
        }

        PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        BIO_free(bio);
        EVP_PKEY_free(pkey);
        return true;
    }

    std::string SshHostKeyProvider::default_key_path(SshHostKeyType type)
    {
        switch (type) {
        case SshHostKeyType::ED25519:
            return "/etc/ssh/ssh_host_ed25519_key";
        case SshHostKeyType::ECDSA_P256:
            return "/etc/ssh/ssh_host_ecdsa_key";
        case SshHostKeyType::RSA:
            return "/etc/ssh/ssh_host_rsa_key";
        default:
            return "";
        }
    }
}
