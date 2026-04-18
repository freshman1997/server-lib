#include "algorithm/ssh_host_key_algorithm.h"
#include "crypto/ssh_crypto.h"
#include "protocol/ssh_message_codec.h"
#include "buffer/byte_buffer.h"
#include "base/utils/base64.h"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#include <cstring>
#include <memory>

namespace yuan::net::ssh
{
    class SshHkRsa : public SshHostKeyAlgorithm
    {
    public:
        explicit SshHkRsa(const std::string &hash_alg)
            : hash_alg_(hash_alg)
        {
        }

        std::string name() const override
        {
            if (hash_alg_ == "sha512")
                return "rsa-sha2-512";
            if (hash_alg_ == "sha256")
                return "rsa-sha2-256";
            return "ssh-rsa";
        }

        std::vector<uint8_t> public_key_blob() const override
        {
            ByteBuffer buf;
            SshMessageCodec::write_string(buf, "ssh-rsa");
            SshMessageCodec::write_mpint(buf, public_exponent_);
            SshMessageCodec::write_mpint(buf, modulus_);
            std::vector<uint8_t> result(
                reinterpret_cast<const uint8_t *>(buf.read_ptr()),
                reinterpret_cast<const uint8_t *>(buf.read_ptr()) + buf.readable_bytes());
            return result;
        }

        std::vector<uint8_t> sign(const std::vector<uint8_t> &data) override
        {
            if (!crypto_ || private_key_.empty())
                return {};
            auto sig_raw = crypto_->rsa_sign(private_key_, hash_alg_, data.data(), data.size());
            if (sig_raw.empty())
                return {};

            ByteBuffer buf;
            SshMessageCodec::write_string(buf, name());
            SshMessageCodec::write_string(buf, std::string(
                reinterpret_cast<const char *>(sig_raw.data()),
                sig_raw.size()));
            std::vector<uint8_t> result(
                reinterpret_cast<const uint8_t *>(buf.read_ptr()),
                reinterpret_cast<const uint8_t *>(buf.read_ptr()) + buf.readable_bytes());
            return result;
        }

        bool verify(const std::vector<uint8_t> &data,
                    const std::vector<uint8_t> &signature) override
        {
            if (!crypto_ || public_key_.empty())
                return false;

            size_t offset = 0;
            auto sig_type = SshMessageCodec::read_string(signature.data(), signature.size(), offset);
            if (!sig_type)
                return false;

            auto sig_blob = SshMessageCodec::read_string(signature.data(), signature.size(), offset);
            if (!sig_blob)
                return false;

            return crypto_->rsa_verify(public_key_, hash_alg_,
                                       data.data(), data.size(),
                                       reinterpret_cast<const uint8_t *>(sig_blob->data()),
                                       sig_blob->size());
        }

        std::string fingerprint() const override
        {
            if (!crypto_ || modulus_.empty())
                return "";
            auto blob = public_key_blob();
            auto hash = crypto_->sha256(blob.data(), blob.size());
            return "SHA256:" + yuan::base::util::base64_encode(hash);
        }

        void set_keys(const std::vector<uint8_t> &pub_der,
                      const std::vector<uint8_t> &priv_der,
                      const std::vector<uint8_t> &exponent,
                      const std::vector<uint8_t> &mod)
        {
            public_key_ = pub_der;
            private_key_ = priv_der;
            public_exponent_ = exponent;
            modulus_ = mod;
        }

        void set_crypto(SshCrypto *crypto) override
        {
            crypto_ = crypto;
        }

        bool load_key_pair(const std::vector<uint8_t> &private_key_der,
                           const std::vector<uint8_t> &public_key_der) override
        {
            if (!private_key_der.empty()) {
                const uint8_t *p = private_key_der.data();
                EVP_PKEY *pkey = d2i_AutoPrivateKey(nullptr, &p, static_cast<long>(private_key_der.size()));
                if (!pkey)
                    return false;

                BIGNUM *n = nullptr;
                BIGNUM *e = nullptr;
                EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n);
                EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e);

                modulus_.resize(BN_num_bytes(n));
                BN_bn2bin(n, modulus_.data());
                public_exponent_.resize(BN_num_bytes(e));
                BN_bn2bin(e, public_exponent_.data());

                private_key_ = private_key_der;

                uint8_t *pub_der = nullptr;
                int pub_len = i2d_PUBKEY(pkey, &pub_der);
                public_key_.assign(pub_der, pub_der + pub_len);
                OPENSSL_free(pub_der);

                BN_free(n);
                BN_free(e);
                EVP_PKEY_free(pkey);
                return true;
            }

            if (!public_key_der.empty()) {
                public_key_ = public_key_der;

                const uint8_t *pp = public_key_der.data();
                EVP_PKEY *pkey = d2i_PUBKEY(nullptr, &pp, static_cast<long>(public_key_der.size()));
                if (pkey) {
                    BIGNUM *n = nullptr;
                    BIGNUM *e = nullptr;
                    EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n);
                    EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e);

                    modulus_.resize(BN_num_bytes(n));
                    BN_bn2bin(n, modulus_.data());
                    public_exponent_.resize(BN_num_bytes(e));
                    BN_bn2bin(e, public_exponent_.data());

                    BN_free(n);
                    BN_free(e);
                    EVP_PKEY_free(pkey);
                }
                return true;
            }

            return false;
        }

    private:
        std::string hash_alg_;
        SshCrypto *crypto_ = nullptr;
        std::vector<uint8_t> public_key_;
        std::vector<uint8_t> private_key_;
        std::vector<uint8_t> public_exponent_;
        std::vector<uint8_t> modulus_;
    };

    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_rsa_sha512()
    {
        return std::make_unique<SshHkRsa>("sha512");
    }

    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_rsa_sha256()
    {
        return std::make_unique<SshHkRsa>("sha256");
    }

    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_rsa()
    {
        return std::make_unique<SshHkRsa>("sha1");
    }
}
