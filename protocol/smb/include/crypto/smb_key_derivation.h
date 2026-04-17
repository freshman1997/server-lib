#ifndef __NET_SMB_CRYPTO_SMB_KEY_DERIVATION_H__
#define __NET_SMB_CRYPTO_SMB_KEY_DERIVATION_H__

#include "protocol/smb2_constants.h"
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::smb
{
    class SmbKeyDerivation
    {
    public:
        static std::vector<uint8_t> derive_signing_key(const std::vector<uint8_t> &session_key,
                                                       DialectRevision dialect,
                                                       const std::vector<uint8_t> &preauth_hash = {});
        static std::vector<uint8_t> derive_encryption_key(const std::vector<uint8_t> &session_key,
                                                          DialectRevision dialect,
                                                          const std::vector<uint8_t> &preauth_hash = {});
        static std::vector<uint8_t> derive_decryption_key(const std::vector<uint8_t> &session_key,
                                                          DialectRevision dialect,
                                                          const std::vector<uint8_t> &preauth_hash = {});
        static std::vector<uint8_t> derive_encryption_iv(const std::vector<uint8_t> &session_key,
                                                         DialectRevision dialect,
                                                         const std::vector<uint8_t> &preauth_hash = {});
        static std::vector<uint8_t> derive_decryption_iv(const std::vector<uint8_t> &session_key,
                                                         DialectRevision dialect,
                                                         const std::vector<uint8_t> &preauth_hash = {});
        static std::vector<uint8_t> sp800_108(const std::vector<uint8_t> &key,
                                              const std::string &label,
                                              const std::vector<uint8_t> &context);
        static std::vector<uint8_t> kdf_sha512(const std::vector<uint8_t> &key,
                                               const std::string &label,
                                               const std::vector<uint8_t> &context);
        static std::vector<uint8_t> compute_preauth_hash(const std::vector<uint8_t> &current_hash,
                                                         const uint8_t *data, size_t len);
    };
}
#endif
