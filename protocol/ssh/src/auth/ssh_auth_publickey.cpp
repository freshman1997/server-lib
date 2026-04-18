#include "auth/ssh_auth_publickey.h"
#include "protocol/ssh_message_codec.h"
#include "ssh_session.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace yuan::net::ssh
{
    namespace
    {
        std::vector<uint8_t> encode_public_key_der(EVP_PKEY * pkey)
        {
            if (!pkey) {
                return {};
            }

            uint8_t * der = nullptr;
            const int der_len = i2d_PUBKEY(pkey, &der);
            if (der_len <= 0 || !der) {
                return {};
            }

            std::vector<uint8_t> result(der, der + der_len);
            OPENSSL_free(der);
            return result;
        }

        bool verify_rsa_signature_from_blob(const std::vector<uint8_t> & public_key_blob,
                                            const std::string & algorithm,
                                            const uint8_t * data,
                                            size_t data_len,
                                            const uint8_t * signature,
                                            size_t signature_len)
        {
            size_t offset = 0;
            auto key_type = SshMessageCodec::read_string(public_key_blob.data(), public_key_blob.size(), offset);
            auto exponent = SshMessageCodec::read_mpint(public_key_blob.data(), public_key_blob.size(), offset);
            auto modulus = SshMessageCodec::read_mpint(public_key_blob.data(), public_key_blob.size(), offset);
            if (!key_type || *key_type != "ssh-rsa" || !exponent || !modulus) {
                return false;
            }

            BIGNUM * e = BN_bin2bn(exponent->data(), static_cast<int>(exponent->size()), nullptr);
            BIGNUM * n = BN_bin2bn(modulus->data(), static_cast<int>(modulus->size()), nullptr);
            if (!e || !n) {
                BN_free(e);
                BN_free(n);
                return false;
            }

            const EVP_MD * md = nullptr;
            if (algorithm == "rsa-sha2-512") {
                md = EVP_sha512();
            } else if (algorithm == "rsa-sha2-256") {
                md = EVP_sha256();
            } else if (algorithm == "ssh-rsa") {
                md = EVP_sha1();
            } else {
                BN_free(e);
                BN_free(n);
                return false;
            }

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
            RSA * rsa = RSA_new();
            EVP_PKEY * pkey = nullptr;
            EVP_MD_CTX * md_ctx = nullptr;
            bool verified = false;
            if (rsa && RSA_set0_key(rsa, n, e, nullptr) == 1) {
                pkey = EVP_PKEY_new();
                if (pkey && EVP_PKEY_assign_RSA(pkey, rsa) == 1) {
                    rsa = nullptr;
                    md_ctx = EVP_MD_CTX_new();
                    if (md_ctx && EVP_DigestVerifyInit(md_ctx, nullptr, md, nullptr, pkey) == 1) {
                        verified = EVP_DigestVerify(md_ctx, signature, signature_len, data, data_len) == 1;
                    }
                }
            }
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

            EVP_MD_CTX_free(md_ctx);
            EVP_PKEY_free(pkey);
            if (rsa) {
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
                RSA_free(rsa);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
                BN_free(e);
                BN_free(n);
            }
            return verified;
        }

        std::vector<uint8_t> decode_ecdsa_public_key_der(const std::vector<uint8_t> & public_key_blob,
                                                          const std::string & expected_algorithm,
                                                          std::string & curve_out)
        {
            size_t offset = 0;
            auto algorithm = SshMessageCodec::read_string(public_key_blob.data(), public_key_blob.size(), offset);
            auto curve_name = SshMessageCodec::read_string(public_key_blob.data(), public_key_blob.size(), offset);
            auto public_point = SshMessageCodec::read_string(public_key_blob.data(), public_key_blob.size(), offset);
            if (!algorithm || *algorithm != expected_algorithm || !curve_name || !public_point) {
                return {};
            }

            const char * group_name = nullptr;
            if (*curve_name == "nistp256") {
                group_name = "prime256v1";
                curve_out = "P-256";
            } else if (*curve_name == "nistp384") {
                group_name = "secp384r1";
                curve_out = "P-384";
            } else if (*curve_name == "nistp521") {
                group_name = "secp521r1";
                curve_out = "P-521";
            } else {
                return {};
            }

            OSSL_PARAM params[3];
            params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
                                                         const_cast<char *>(group_name), 0);
            params[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY,
                                                          const_cast<char *>(public_point->data()),
                                                          public_point->size());
            params[2] = OSSL_PARAM_construct_end();

            EVP_PKEY_CTX * ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
            EVP_PKEY * pkey = nullptr;
            std::vector<uint8_t> der;
            if (ctx && EVP_PKEY_fromdata_init(ctx) > 0 &&
                EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) > 0) {
                der = encode_public_key_der(pkey);
            }

            EVP_PKEY_free(pkey);
            EVP_PKEY_CTX_free(ctx);
            return der;
        }
    }

    SshAuthPublickey::SshAuthPublickey(SshCrypto * crypto)
        : crypto_(crypto)
    {
    }

    SshAuthResult SshAuthPublickey::authenticate(SshSession * session,
                                                 const std::string & username,
                                                 const SshAuthCredentials & credentials)
    {
        if (!credentials.has_signature)
            return SshAuthResult::NEED_MORE;

        if (credentials.signature.empty() || credentials.public_key_blob.empty())
            return SshAuthResult::FAILURE;

        if (!session)
            return SshAuthResult::FAILURE;

        const auto &session_id = session->session_id_proto();
        return verify_signature(session_id, username,
                                credentials.public_key_algorithm,
                                credentials.public_key_blob,
                                credentials.signature)
                   ? SshAuthResult::SUCCESS
                   : SshAuthResult::FAILURE;
    }

    bool SshAuthPublickey::verify_signature(const std::vector<uint8_t> & session_id,
                                            const std::string & username,
                                            const std::string & algorithm,
                                            const std::vector<uint8_t> & public_key_blob,
                                            const std::vector<uint8_t> & signature)
    {
        if (!crypto_)
            return false;

        ByteBuffer signed_data;
        SshMessageCodec::write_raw(signed_data, session_id.data(), session_id.size());
        signed_data.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_REQUEST));
        SshMessageCodec::write_string(signed_data, username);
        SshMessageCodec::write_string(signed_data, SSH_SERVICE_CONNECTION);
        SshMessageCodec::write_string(signed_data, "publickey");
        SshMessageCodec::write_boolean(signed_data, true);
        SshMessageCodec::write_string(signed_data, algorithm);
        SshMessageCodec::write_raw(signed_data, public_key_blob.data(), public_key_blob.size());

        size_t sig_offset = 0;
        auto signature_blob = SshMessageCodec::read_string(signature.data(), signature.size(), sig_offset);
        if (!signature_blob) {
            return false;
        }

        sig_offset = 0;
        auto signature_algorithm = SshMessageCodec::read_string(
            reinterpret_cast<const uint8_t *>(signature_blob->data()), signature_blob->size(), sig_offset);
        if (!signature_algorithm || *signature_algorithm != algorithm) {
            return false;
        }

        auto signature_value = SshMessageCodec::read_string(
            reinterpret_cast<const uint8_t *>(signature_blob->data()), signature_blob->size(), sig_offset);
        if (!signature_value) {
            return false;
        }

        size_t key_offset = 0;
        auto key_algorithm = SshMessageCodec::read_string(public_key_blob.data(), public_key_blob.size(), key_offset);
        if (!key_algorithm) {
            return false;
        }

        const bool rsa_algorithm_match =
            (*key_algorithm == "ssh-rsa") &&
            (algorithm == "ssh-rsa" || algorithm == "rsa-sha2-256" || algorithm == "rsa-sha2-512");
        if (*key_algorithm != algorithm && !rsa_algorithm_match) {
            return false;
        }

        const auto * signed_ptr = reinterpret_cast<const uint8_t *>(signed_data.read_ptr());
        const auto signed_len = signed_data.readable_bytes();
        const auto * sig_ptr = reinterpret_cast<const uint8_t *>(signature_value->data());
        const auto sig_len = signature_value->size();

        if (algorithm == "ssh-ed25519") {
            auto raw_public_key = SshMessageCodec::read_string(public_key_blob.data(), public_key_blob.size(), key_offset);
            if (!raw_public_key) {
                return false;
            }
            return crypto_->ed25519_verify(
                std::vector<uint8_t>(raw_public_key->begin(), raw_public_key->end()),
                signed_ptr, signed_len, sig_ptr, sig_len);
        }

        if (algorithm == "ssh-rsa" || algorithm == "rsa-sha2-256" || algorithm == "rsa-sha2-512") {
            return verify_rsa_signature_from_blob(public_key_blob, algorithm, signed_ptr, signed_len, sig_ptr, sig_len);
        }

        if (algorithm.rfind("ecdsa-sha2-", 0) == 0) {
            std::string curve;
            auto ecdsa_public_key_der = decode_ecdsa_public_key_der(public_key_blob, algorithm, curve);
            if (ecdsa_public_key_der.empty() || curve.empty()) {
                return false;
            }

            return crypto_->ecdsa_verify(ecdsa_public_key_der, curve, signed_ptr, signed_len, sig_ptr, sig_len);
        }

        return false;
    }
}
