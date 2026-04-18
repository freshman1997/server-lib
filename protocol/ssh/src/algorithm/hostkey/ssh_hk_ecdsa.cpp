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
    class SshHkEcdsa : public SshHostKeyAlgorithm
    {
    public:
        explicit SshHkEcdsa(const std::string &curve)
            : curve_(curve)
        {
        }

        std::string name() const override
        {
            if (curve_ == "P-256")
                return "ecdsa-sha2-nistp256";
            if (curve_ == "P-384")
                return "ecdsa-sha2-nistp384";
            if (curve_ == "P-521")
                return "ecdsa-sha2-nistp521";
            return "ecdsa-sha2-nistp256";
        }

        std::vector<uint8_t> public_key_blob() const override
        {
            std::string curve_name;
            if (curve_ == "P-256")
                curve_name = "nistp256";
            else if (curve_ == "P-384")
                curve_name = "nistp384";
            else if (curve_ == "P-521")
                curve_name = "nistp521";
            else
                curve_name = "nistp256";

            ByteBuffer buf;
            SshMessageCodec::write_string(buf, name());
            SshMessageCodec::write_string(buf, curve_name);
            SshMessageCodec::write_string(buf, std::string(
                reinterpret_cast<const char *>(public_key_.data()),
                public_key_.size()));
            std::vector<uint8_t> result(
                reinterpret_cast<const uint8_t *>(buf.read_ptr()),
                reinterpret_cast<const uint8_t *>(buf.read_ptr()) + buf.readable_bytes());
            return result;
        }

        std::vector<uint8_t> sign(const std::vector<uint8_t> &data) override
        {
            if (!crypto_ || private_key_.empty())
                return {};
            auto sig_raw = crypto_->ecdsa_sign(private_key_, curve_, data.data(), data.size());
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

            return crypto_->ecdsa_verify(public_key_, curve_,
                                         data.data(), data.size(),
                                         reinterpret_cast<const uint8_t *>(sig_blob->data()),
                                         sig_blob->size());
        }

        std::string fingerprint() const override
        {
            if (!crypto_ || public_key_.empty())
                return "";
            auto blob = public_key_blob();
            auto hash = crypto_->sha256(blob.data(), blob.size());
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

                BIGNUM *pub_x = nullptr;
                BIGNUM *pub_y = nullptr;
                EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_EC_PUB_X, &pub_x);
                EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_EC_PUB_Y, &pub_y);

                size_t coord_len = (curve_ == "P-256") ? 32
                                                       : (curve_ == "P-384") ? 48 : 66;

                public_key_.resize(1 + 2 * coord_len);
                public_key_[0] = 0x04;
                BN_bn2binpad(pub_x, public_key_.data() + 1, static_cast<int>(coord_len));
                BN_bn2binpad(pub_y, public_key_.data() + 1 + coord_len, static_cast<int>(coord_len));

                private_key_ = private_key_der;

                BN_free(pub_x);
                BN_free(pub_y);
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
        std::string curve_;
        SshCrypto *crypto_ = nullptr;
        std::vector<uint8_t> public_key_;
        std::vector<uint8_t> private_key_;
    };

    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_ecdsa_nistp256()
    {
        return std::make_unique<SshHkEcdsa>("P-256");
    }

    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_ecdsa_nistp384()
    {
        return std::make_unique<SshHkEcdsa>("P-384");
    }

    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_ecdsa_nistp521()
    {
        return std::make_unique<SshHkEcdsa>("P-521");
    }
}
