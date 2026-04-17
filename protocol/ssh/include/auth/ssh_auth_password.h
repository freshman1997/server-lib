#ifndef __NET_SSH_AUTH_SSH_AUTH_PASSWORD_H__
#define __NET_SSH_AUTH_SSH_AUTH_PASSWORD_H__

#include "auth/ssh_auth_method.h"

namespace yuan::net::ssh
{
    class SshAuthPassword : public SshAuthMethod
    {
    public:
        std::string name() const override
        {
            return "password";
        }

        SshAuthResult authenticate(SshSession *session,
                                   const std::string &username,
                                   const SshAuthCredentials &credentials) override
        {
            (void)session;
            (void)username;
            if (credentials.password.empty())
                return SshAuthResult::FAILURE;
            return SshAuthResult::SUCCESS;
        }
    };
}

#endif
