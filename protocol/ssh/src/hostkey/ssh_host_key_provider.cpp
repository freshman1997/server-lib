#include "hostkey/ssh_host_key_provider.h"
#include "crypto/ssh_crypto.h"
#include "crypto/ssh_crypto_openssl.h"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace yuan::net::ssh
{
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_ed25519();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_rsa_sha512();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_rsa_sha256();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_rsa();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_ecdsa_nistp256();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_ecdsa_nistp384();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_ecdsa_nistp521();

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

        EVP_PKEY *generate_pkey(SshHostKeyType type)
        {
            const char *algorithm = nullptr;
            OSSL_PARAM params[2];
            params[0] = OSSL_PARAM_construct_end();
            params[1] = OSSL_PARAM_construct_end();

            size_t bits_value = 3072;
            char p256_name[] = "prime256v1";
            char p384_name[] = "secp384r1";
            char p521_name[] = "secp521r1";

            switch (type) {
            case SshHostKeyType::ED25519:
                algorithm = "ED25519";
                break;
            case SshHostKeyType::ECDSA_P256:
                algorithm = "EC";
                params[0] = OSSL_PARAM_construct_utf8_string(
                    OSSL_PKEY_PARAM_GROUP_NAME, p256_name, 0);
                break;
            case SshHostKeyType::ECDSA_P384:
                algorithm = "EC";
                params[0] = OSSL_PARAM_construct_utf8_string(
                    OSSL_PKEY_PARAM_GROUP_NAME, p384_name, 0);
                break;
            case SshHostKeyType::ECDSA_P521:
                algorithm = "EC";
                params[0] = OSSL_PARAM_construct_utf8_string(
                    OSSL_PKEY_PARAM_GROUP_NAME, p521_name, 0);
                break;
            case SshHostKeyType::RSA:
                algorithm = "RSA";
                params[0] = OSSL_PARAM_construct_size_t(OSSL_PKEY_PARAM_BITS, &bits_value);
                break;
            default:
                return nullptr;
            }

            EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(nullptr, algorithm, nullptr);
            if (!ctx)
                return nullptr;

            EVP_PKEY *pkey = nullptr;
            if (EVP_PKEY_keygen_init(ctx) <= 0) {
                EVP_PKEY_CTX_free(ctx);
                return nullptr;
            }

            if (params[0].key != nullptr && EVP_PKEY_CTX_set_params(ctx, params) <= 0) {
                EVP_PKEY_CTX_free(ctx);
                return nullptr;
            }

            if (EVP_PKEY_generate(ctx, &pkey) <= 0) {
                EVP_PKEY_CTX_free(ctx);
                return nullptr;
            }

            EVP_PKEY_CTX_free(ctx);
            return pkey;
        }
    }

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
        EVP_PKEY *pkey = generate_pkey(type);
        if (!pkey)
            return false;

        std::error_code ec;
        const std::filesystem::path key_path(path);
        if (key_path.has_parent_path())
            std::filesystem::create_directories(key_path.parent_path(), ec);

        BIO *bio = BIO_new_file(path.c_str(), "w");
        if (!bio) {
            EVP_PKEY_free(pkey);
            return false;
        }

        const bool wrote_key = PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1;
        BIO_free(bio);
        EVP_PKEY_free(pkey);
        return wrote_key;
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
