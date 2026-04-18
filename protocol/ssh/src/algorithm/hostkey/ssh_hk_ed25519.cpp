#include "algorithm/ssh_host_key_algorithm.h"
#include "crypto/ssh_crypto.h"
#include "protocol/ssh_message_codec.h"
#include "buffer/byte_buffer.h"
#include "base/utils/base64.h"
#include "openssl/evp.h"
#include "openssl/pem.h"
#include "openssl/bio.h"
#include "openssl/core_names.h"
#include <cstring>
#include <memory>

namespace yuan::net::ssh
{
    class SshHkEd25519 : public SshHostKeyAlgorithm
    {
    public:
        SshHkEd25519() = default;

        std::string name() const override
        {
            return "ssh-ed25519";
        }

        std::vector<uint8_t> public_key_blob() const override
        {
            ByteBuffer buf;
            SshMessageCodec::write_string(buf, "ssh-ed25519");
            SshMessageCodec::write_raw(buf, public_key_.data(), public_key_.size());
            std::vector<uint8_t> result(
                reinterpret_cast<const uint8_t *>(buf.read_ptr()),
                reinterpret_cast<const uint8_t *>(buf.read_ptr()) + buf.readable_bytes());
            return result;
        }

        std::vector<uint8_t> sign(const std::vector<uint8_t> &data) override
        {
            if (!crypto_ || private_key_.empty())
                return {};
            auto sig_raw = crypto_->ed25519_sign(private_key_, data.data(), data.size());
            if (sig_raw.empty())
                return {};

            ByteBuffer buf;
            SshMessageCodec::write_string(buf, "ssh-ed25519");
            SshMessageCodec::write_raw(buf, sig_raw.data(), sig_raw.size());
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
            if (!sig_type || *sig_type != "ssh-ed25519")
                return false;

            auto sig_blob = SshMessageCodec::read_string(signature.data(), signature.size(), offset);
            if (!sig_blob)
                return false;

            return crypto_->ed25519_verify(public_key_,
                                           data.data(), data.size(),
                                           reinterpret_cast<const uint8_t *>(sig_blob->data()),
                                           sig_blob->size());
        }

        std::string fingerprint() const override
        {
            if (!crypto_ || public_key_.empty())
                return "";
            auto hash = crypto_->sha256(public_key_.data(), public_key_.size());
            return "SHA256:" + yuan::base::util::base64_encode(hash);
        }

        void set_keys(const std::vector<uint8_t> &pub, const std::vector<uint8_t> &priv)
        {
            public_key_ = pub;
            private_key_ = priv;
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

                size_t pub_len = 32;
                public_key_.resize(pub_len);
                if (EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PUB_KEY,
                                                    public_key_.data(), pub_len, &pub_len) != 1) {
                    EVP_PKEY_free(pkey);
                    return false;
                }
                public_key_.resize(pub_len);

                size_t priv_len = 32;
                private_key_.resize(priv_len);
                if (EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PRIV_KEY,
                                                    private_key_.data(), priv_len, &priv_len) != 1) {
                    EVP_PKEY_free(pkey);
                    return false;
                }
                private_key_.resize(priv_len);

                EVP_PKEY_free(pkey);
                return true;
            }

            if (!public_key_der.empty()) {
                public_key_ = public_key_der;
                return true;
            }

            return false;
        }

    private:
        SshCrypto *crypto_ = nullptr;
        std::vector<uint8_t> public_key_;
        std::vector<uint8_t> private_key_;
    };

    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_ed25519()
    {
        return std::make_unique<SshHkEd25519>();
    }
}
