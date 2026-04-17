#ifndef __NET_SSH_AUTH_SSH_AUTH_PUBLICKEY_H__
#define __NET_SSH_AUTH_SSH_AUTH_PUBLICKEY_H__

#include "auth/ssh_auth_method.h"
#include "crypto/ssh_crypto.h"
#include <memory>

namespace yuan::net::ssh
{
    class SshAuthPublickey : public SshAuthMethod
    {
    public:
        explicit SshAuthPublickey(SshCrypto *crypto = nullptr);

        std::string name() const override
        {
            return "publickey";
        }

        SshAuthResult authenticate(SshSession *session,
                                   const std::string &username,
                                   const SshAuthCredentials &credentials) override;

        bool verify_signature(const std::vector<uint8_t> &session_id,
                              const std::string &username,
                              const std::string &algorithm,
                              const std::vector<uint8_t> &public_key_blob,
                              const std::vector<uint8_t> &signature);

    private:
        SshCrypto *crypto_ = nullptr;
    };
}

#endif
