#ifndef __NET_SSH_AUTH_SSH_AUTH_KEYBOARD_INTERACTIVE_H__
#define __NET_SSH_AUTH_SSH_AUTH_KEYBOARD_INTERACTIVE_H__

#include "auth/ssh_auth_method.h"

namespace yuan::net::ssh
{
    class SshAuthKeyboardInteractive : public SshAuthMethod
    {
    public:
        std::string name() const override
        {
            return "keyboard-interactive";
        }

        bool needs_more() const override
        {
            return true;
        }

        SshAuthResult authenticate(SshSession *session,
                                   const std::string &username,
                                   const SshAuthCredentials &credentials) override
        {
            (void)session;
            (void)username;
            (void)credentials;
            return SshAuthResult::NEED_MORE;
        }

        SshUserauthInfoRequestMessage build_challenge(SshSession *session,
                                                      const std::string &username) override
        {
            (void)session;
            (void)username;
            SshUserauthInfoRequestMessage msg;
            msg.name = "Keyboard Interactive Authentication";
            msg.instruction = "";
            msg.language = "en";
            SshAuthPrompt prompt;
            prompt.prompt = "Password: ";
            prompt.echo = false;
            msg.prompts.push_back(std::move(prompt));
            return msg;
        }

        SshAuthResult process_response(SshSession *session,
                                       const std::string &username,
                                       const std::vector<std::string> &responses) override
        {
            (void)session;
            (void)username;
            (void)responses;
            return SshAuthResult::FAILURE;
        }
    };
}

#endif
