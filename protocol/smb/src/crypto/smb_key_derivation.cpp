#include "crypto/smb_key_derivation.h"
#include "crypto/smb_crypto_openssl.h"
#include <cstring>

namespace yuan::net::smb
{
    static std::vector<uint8_t> hmac_sha256_internal(const std::vector<uint8_t> & key, const uint8_t * data, size_t len)
    {
        SmbCryptoOpenSSL crypto;
        return crypto.hmac_sha256(key, data, len);
    }

    std::vector<uint8_t> SmbKeyDerivation::sp800_108(const std::vector<uint8_t> & key,
                                                     const std::string & label,
                                                     const std::vector<uint8_t> & context)
    {
        std::vector<uint8_t> input;
        uint8_t counter[4] = { 0x00, 0x00, 0x00, 0x01 };
        input.insert(input.end(), counter, counter + 4);
        input.insert(input.end(), label.begin(), label.end());
        input.push_back(0x00);
        input.insert(input.end(), context.begin(), context.end());
        uint8_t length[4] = { 0x00, 0x00, 0x01, 0x00 };
        input.insert(input.end(), length, length + 4);

        auto result = hmac_sha256_internal(key, input.data(), input.size());
        result.resize(16);
        return result;
    }

    std::vector<uint8_t> SmbKeyDerivation::kdf_sha512(const std::vector<uint8_t> & key,
                                                      const std::string & label,
                                                      const std::vector<uint8_t> & context)
    {
        std::vector<uint8_t> input;
        input.insert(input.end(), label.begin(), label.end());
        input.insert(input.end(), context.begin(), context.end());

        SmbCryptoOpenSSL crypto;
        auto hash = crypto.sha512(input.data(), input.size());

        std::vector<uint8_t> result(16);
        std::memcpy(result.data(), hash.data(), 16);
        return result;
    }

    std::vector<uint8_t> SmbKeyDerivation::compute_preauth_hash(const std::vector<uint8_t> & current_hash,
                                                                const uint8_t * data, size_t len)
    {
        SmbCryptoOpenSSL crypto;
        std::vector<uint8_t> combined;
        combined.reserve(current_hash.size() + len);
        combined.insert(combined.end(), current_hash.begin(), current_hash.end());
        combined.insert(combined.end(), data, data + len);
        return crypto.sha512(combined.data(), combined.size());
    }

    std::vector<uint8_t> SmbKeyDerivation::derive_signing_key(const std::vector<uint8_t> & session_key,
                                                              DialectRevision dialect,
                                                              const std::vector<uint8_t> & preauth_hash)
    {
        if (dialect == DialectRevision::SMB_2_002 || dialect == DialectRevision::SMB_2_1) {
            std::vector<uint8_t> input;
            const char *label = "SMB2APPKEY";
            input.insert(input.end(), label, label + 10);
            input.insert(input.end(), session_key.begin(), session_key.end());
            return hmac_sha256_internal(session_key, input.data(), input.size());
        }

        if (dialect == DialectRevision::SMB_3_0 || dialect == DialectRevision::SMB_3_0_2) {
            return sp800_108(session_key, "SmbSign", preauth_hash);
        }

        if (dialect == DialectRevision::SMB_3_1_1) {
            return kdf_sha512(session_key, "SMBSigningKey", preauth_hash);
        }

        return {};
    }

    std::vector<uint8_t> SmbKeyDerivation::derive_encryption_key(const std::vector<uint8_t> & session_key,
                                                                 DialectRevision dialect,
                                                                 const std::vector<uint8_t> & preauth_hash)
    {
        if (dialect == DialectRevision::SMB_3_0 || dialect == DialectRevision::SMB_3_0_2) {
            return sp800_108(session_key, "SmbRCEnc", preauth_hash);
        }

        if (dialect == DialectRevision::SMB_3_1_1) {
            return kdf_sha512(session_key, "SMBC2SCipherKey", preauth_hash);
        }

        return {};
    }

    std::vector<uint8_t> SmbKeyDerivation::derive_decryption_key(const std::vector<uint8_t> & session_key,
                                                                 DialectRevision dialect,
                                                                 const std::vector<uint8_t> & preauth_hash)
    {
        if (dialect == DialectRevision::SMB_3_0 || dialect == DialectRevision::SMB_3_0_2) {
            return sp800_108(session_key, "SmbRCDec", preauth_hash);
        }

        if (dialect == DialectRevision::SMB_3_1_1) {
            return kdf_sha512(session_key, "SMBS2CCipherKey", preauth_hash);
        }

        return {};
    }

    std::vector<uint8_t> SmbKeyDerivation::derive_encryption_iv(const std::vector<uint8_t> & session_key,
                                                                DialectRevision dialect,
                                                                const std::vector<uint8_t> & preauth_hash)
    {
        if (dialect == DialectRevision::SMB_3_0 || dialect == DialectRevision::SMB_3_0_2) {
            auto iv = sp800_108(session_key, "SmbRCEnc", preauth_hash);
            iv.resize(16);
            return iv;
        }

        if (dialect == DialectRevision::SMB_3_1_1) {
            auto iv = kdf_sha512(session_key, "SMBC2SIV", preauth_hash);
            iv.resize(16);
            return iv;
        }

        return {};
    }

    std::vector<uint8_t> SmbKeyDerivation::derive_decryption_iv(const std::vector<uint8_t> & session_key,
                                                                DialectRevision dialect,
                                                                const std::vector<uint8_t> & preauth_hash)
    {
        if (dialect == DialectRevision::SMB_3_0 || dialect == DialectRevision::SMB_3_0_2) {
            auto iv = sp800_108(session_key, "SmbRCDec", preauth_hash);
            iv.resize(16);
            return iv;
        }

        if (dialect == DialectRevision::SMB_3_1_1) {
            auto iv = kdf_sha512(session_key, "SMBS2CIV", preauth_hash);
            iv.resize(16);
            return iv;
        }

        return {};
    }
}
