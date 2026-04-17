#ifndef __NET_SSH_AUTH_SSH_AUTHENTICATOR_H__
#define __NET_SSH_AUTH_SSH_AUTHENTICATOR_H__

#include "auth/ssh_auth_method.h"
#include "protocol/ssh_constants.h"
#include "protocol/ssh_structures.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net::ssh
{
    class SshSession;
    class SshHandler;

    class SshAuthenticator
    {
    public:
        enum class State {
            none,
            service_requested,
            authenticating,
            authenticated,
            failed
        };

        enum class PendingAuthResponse {
            none,
            pk_ok,
            info_request
        };

        explicit SshAuthenticator(uint32_t max_attempts = SSH_MAX_AUTH_ATTEMPTS);

        void register_method(std::unique_ptr<SshAuthMethod> method);

        State state() const
        {
            return state_;
        }
        bool authenticated() const
        {
            return state_ == State::authenticated;
        }
        const std::string &username() const
        {
            return username_;
        }
        uint32_t auth_attempts() const
        {
            return auth_attempts_;
        }
        PendingAuthResponse pending_auth_response() const
        {
            return pending_auth_response_;
        }
        const std::string &pending_pk_algo() const
        {
            return pending_pk_algo_;
        }
        const std::vector<uint8_t> &pending_pk_key_blob() const
        {
            return pending_pk_key_blob_;
        }
        const std::string &current_method() const
        {
            return current_method_;
        }
        SshAuthMethod *active_method() const
        {
            return active_method_;
        }

        bool process_service_request(const std::string &service_name);
        SshAuthResult process_userauth_request(SshSession *session,
                                               SshHandler *handler,
                                               const SshUserauthRequestMessage &msg);
        SshAuthResult process_info_response(SshSession *session,
                                            SshHandler *handler,
                                            const SshUserauthInfoResponseMessage &msg);

        std::string allowed_methods_string() const;

        bool max_attempts_exceeded() const;

        void reset();

    private:
        State state_ = State::none;
        std::string username_;
        uint32_t auth_attempts_ = 0;
        uint32_t max_attempts_;
        bool partial_success_ = false;
        std::string current_method_;
        SshAuthMethod *active_method_ = nullptr;

        PendingAuthResponse pending_auth_response_ = PendingAuthResponse::none;
        std::string pending_pk_algo_;
        std::vector<uint8_t> pending_pk_key_blob_;

        std::vector<std::string> method_order_;
        std::unordered_map<std::string, std::unique_ptr<SshAuthMethod> > methods_;
    };
}

#endif
