#ifndef __NET_SSH_AUTH_SSH_AUTH_METHOD_H__
#define __NET_SSH_AUTH_SSH_AUTH_METHOD_H__

#include "protocol/ssh_constants.h"
#include "protocol/ssh_structures.h"
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    class SshSession;

    class SshAuthMethod
    {
    public:
        virtual ~SshAuthMethod() = default;

        virtual std::string name() const = 0;

        virtual SshAuthResult authenticate(SshSession *session,
                                           const std::string &username,
                                           const SshAuthCredentials &credentials) = 0;

        virtual bool needs_more() const
        {
            return false;
        }

        virtual SshUserauthInfoRequestMessage build_challenge(SshSession *session,
                                                              const std::string &username)
        {
            return {};
        }

        virtual SshAuthResult process_response(SshSession *session,
                                               const std::string &username,
                                               const std::vector<std::string> &responses)
        {
            return SshAuthResult::FAILURE;
        }
    };
}

#endif
