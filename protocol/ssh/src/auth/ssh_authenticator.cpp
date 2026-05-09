#include "auth/ssh_authenticator.h"
#include "ssh_handler.h"
#include "ssh_session.h"

#include "logger.h"
#include "net/socket/inet_address.h"

namespace yuan::net::ssh
{
    namespace
    {
        const char * auth_result_name(SshAuthResult result)
        {
            switch (result) {
            case SshAuthResult::SUCCESS:
                return "success";
            case SshAuthResult::FAILURE:
                return "failure";
            case SshAuthResult::NEED_MORE:
                return "need_more";
            default:
                return "unknown";
            }
        }

        std::string auth_peer_address(SshSession *session)
        {
            if (!session) {
                return "unknown";
            }
            auto conn = session->client_connection();
            if (!conn) {
                return "unknown";
            }
            return conn->get_remote_address().to_address_key();
        }

        void log_auth_attempt(SshSession *session,
                              const std::string &username,
                              const std::string &method,
                              SshAuthResult result,
                              uint32_t attempts,
                              uint32_t max_attempts)
        {
            const std::string peer = auth_peer_address(session);
            LOG_INFO_TAG("ssh-auth",
                         "event=auth_attempt peer={} user={} method={} result={} attempts={}/{}",
                         peer,
                         username,
                         method,
                         auth_result_name(result),
                         attempts,
                         max_attempts);
        }
    }

    SshAuthenticator::SshAuthenticator(uint32_t max_attempts)
        : max_attempts_(max_attempts)
    {
    }

    void SshAuthenticator::register_method(std::unique_ptr<SshAuthMethod> method)
    {
        std::string n = method->name();
        method_order_.push_back(n);
        methods_[n] = std::move(method);
    }

    bool SshAuthenticator::process_service_request(const std::string & service_name)
    {
        if (service_name != SSH_SERVICE_USERAUTH)
            return false;

        state_ = State::service_requested;
        return true;
    }

    SshAuthResult SshAuthenticator::process_userauth_request(
        SshSession * session,
        SshHandler * handler,
        const SshUserauthRequestMessage & msg)
    {
        auto *effective_handler = handler ? handler : &SshHandler::default_handler();
        pending_auth_response_ = PendingAuthResponse::none;

        if (state_ == State::authenticated)
            return SshAuthResult::SUCCESS;

        if (state_ != State::service_requested && state_ != State::authenticating)
            return SshAuthResult::FAILURE;

        if (msg.service_name != SSH_SERVICE_CONNECTION)
            return SshAuthResult::FAILURE;

        if (!username_.empty() && username_ != msg.username)
            return SshAuthResult::FAILURE;

        if (username_.empty())
            username_ = msg.username;

        current_method_ = msg.method_name;
        auto it = methods_.find(msg.method_name);
        if (it == methods_.end())
            return SshAuthResult::FAILURE;

        active_method_ = it->second ? &*it->second : nullptr;
        state_ = State::authenticating;

        SshAuthCredentials creds;
        if (msg.method_name == "password") {
            size_t offset = 0;
            const auto &data = msg.method_specific_data;
            if (data.size() < 5)
                return SshAuthResult::FAILURE;

            bool has_sig = data[0] != 0;
            offset = 1;

            uint32_t pw_len = (static_cast<uint32_t>(data[offset]) << 24) |
                              (static_cast<uint32_t>(data[offset + 1]) << 16) |
                              (static_cast<uint32_t>(data[offset + 2]) << 8) |
                              static_cast<uint32_t>(data[offset + 3]);
            offset += 4;

            if (offset + pw_len > data.size())
                return SshAuthResult::FAILURE;

            creds.password = std::string(data.begin() + offset, data.begin() + offset + pw_len);
            creds.has_signature = false;
            (void)has_sig;
        } else if (msg.method_name == "publickey") {
            const auto &data = msg.method_specific_data;
            size_t offset = 0;
            if (data.size() < 1)
                return SshAuthResult::FAILURE;

            bool has_sig = data[0] != 0;
            offset = 1;

            if (data.size() < offset + 4)
                return SshAuthResult::FAILURE;

            uint32_t algo_len = (static_cast<uint32_t>(data[offset]) << 24) |
                                (static_cast<uint32_t>(data[offset + 1]) << 16) |
                                (static_cast<uint32_t>(data[offset + 2]) << 8) |
                                static_cast<uint32_t>(data[offset + 3]);
            offset += 4;

            if (offset + algo_len > data.size())
                return SshAuthResult::FAILURE;

            creds.public_key_algorithm = std::string(data.begin() + offset,
                                                     data.begin() + offset + algo_len);
            offset += algo_len;

            if (data.size() < offset + 4)
                return SshAuthResult::FAILURE;

            uint32_t key_len = (static_cast<uint32_t>(data[offset]) << 24) |
                               (static_cast<uint32_t>(data[offset + 1]) << 16) |
                               (static_cast<uint32_t>(data[offset + 2]) << 8) |
                               static_cast<uint32_t>(data[offset + 3]);
            offset += 4;

            if (offset + key_len > data.size())
                return SshAuthResult::FAILURE;

            creds.public_key_blob.assign(data.begin() + offset, data.begin() + offset + key_len);
            offset += key_len;

            creds.has_signature = has_sig;
            if (has_sig && offset < data.size()) {
                creds.signature.assign(data.begin() + offset, data.end());
            }
        } else if (msg.method_name == "keyboard-interactive") {
            const auto &data = msg.method_specific_data;
            size_t offset = 0;

            if (data.size() >= 4) {
                uint32_t lang_len = (static_cast<uint32_t>(data[offset]) << 24) |
                                    (static_cast<uint32_t>(data[offset + 1]) << 16) |
                                    (static_cast<uint32_t>(data[offset + 2]) << 8) |
                                    static_cast<uint32_t>(data[offset + 3]);
                offset += 4;

                if (offset + lang_len <= data.size())
                    offset += lang_len;

                if (offset + 4 <= data.size()) {
                    uint32_t sub_len = (static_cast<uint32_t>(data[offset]) << 24) |
                                       (static_cast<uint32_t>(data[offset + 1]) << 16) |
                                       (static_cast<uint32_t>(data[offset + 2]) << 8) |
                                       static_cast<uint32_t>(data[offset + 3]);
                    offset += 4;

                    if (offset + sub_len <= data.size())
                        creds.kb_interactive_submethods = std::string(
                            data.begin() + offset, data.begin() + offset + sub_len);
                }
            }
        }

        SshAuthResult handler_result = effective_handler->on_authenticate(session, username_, msg.method_name, creds);

        if (handler_result == SshAuthResult::FAILURE && active_method_) {
            handler_result = active_method_->authenticate(session, username_, creds);
        }

        if (handler_result == SshAuthResult::SUCCESS) {
            state_ = State::authenticated;
            pending_auth_response_ = PendingAuthResponse::none;
            log_auth_attempt(session, username_, msg.method_name, SshAuthResult::SUCCESS, auth_attempts_, max_attempts_);
            return SshAuthResult::SUCCESS;
        }

        if (session) {
            session->clear_authorized_key_restrictions();
        }

        if (handler_result == SshAuthResult::NEED_MORE) {
            if (msg.method_name == "publickey" && !creds.has_signature) {
                pending_auth_response_ = PendingAuthResponse::pk_ok;
                pending_pk_algo_ = creds.public_key_algorithm;
                pending_pk_key_blob_ = creds.public_key_blob;
            } else if (msg.method_name == "keyboard-interactive") {
                pending_auth_response_ = PendingAuthResponse::info_request;
            }
            log_auth_attempt(session, username_, msg.method_name, SshAuthResult::NEED_MORE, auth_attempts_, max_attempts_);
            return SshAuthResult::NEED_MORE;
        }

        auth_attempts_++;
        if (max_attempts_exceeded()) {
            state_ = State::failed;
            log_auth_attempt(session, username_, msg.method_name, SshAuthResult::FAILURE, auth_attempts_, max_attempts_);
            return SshAuthResult::FAILURE;
        }

        log_auth_attempt(session, username_, msg.method_name, SshAuthResult::FAILURE, auth_attempts_, max_attempts_);

        return SshAuthResult::FAILURE;
    }

    SshAuthResult SshAuthenticator::process_info_response(
        SshSession * session,
        SshHandler * handler,
        const SshUserauthInfoResponseMessage & msg)
    {
        auto *effective_handler = handler ? handler : &SshHandler::default_handler();
        pending_auth_response_ = PendingAuthResponse::none;

        if (state_ != State::authenticating || !active_method_)
            return SshAuthResult::FAILURE;

        SshAuthCredentials creds;
        creds.kb_interactive_responses = msg.responses;

        SshAuthResult handler_result = effective_handler->on_authenticate(
            session, username_, current_method_, creds);

        if (handler_result == SshAuthResult::FAILURE && active_method_) {
            handler_result = active_method_->process_response(session, username_, msg.responses);
        }

        if (handler_result == SshAuthResult::SUCCESS) {
            state_ = State::authenticated;
            pending_auth_response_ = PendingAuthResponse::none;
            log_auth_attempt(session, username_, current_method_, SshAuthResult::SUCCESS, auth_attempts_, max_attempts_);
            return SshAuthResult::SUCCESS;
        }

        if (handler_result == SshAuthResult::NEED_MORE) {
            if (current_method_ == "keyboard-interactive") {
                pending_auth_response_ = PendingAuthResponse::info_request;
            }
            log_auth_attempt(session, username_, current_method_, SshAuthResult::NEED_MORE, auth_attempts_, max_attempts_);
            return SshAuthResult::NEED_MORE;
        }

        auth_attempts_++;
        if (max_attempts_exceeded()) {
            state_ = State::failed;
            log_auth_attempt(session, username_, current_method_, SshAuthResult::FAILURE, auth_attempts_, max_attempts_);
            return SshAuthResult::FAILURE;
        }

        log_auth_attempt(session, username_, current_method_, SshAuthResult::FAILURE, auth_attempts_, max_attempts_);

        return SshAuthResult::FAILURE;
    }

    std::string SshAuthenticator::allowed_methods_string() const
    {
        std::string result;
        for (size_t i = 0; i < method_order_.size(); ++i) {
            if (i > 0)
                result += ',';
            result += method_order_[i];
        }
        return result;
    }

    bool SshAuthenticator::max_attempts_exceeded() const
    {
        return auth_attempts_ >= max_attempts_;
    }

    void SshAuthenticator::reset()
    {
        state_ = State::none;
        username_.clear();
        auth_attempts_ = 0;
        current_method_.clear();
        active_method_ = nullptr;
        pending_auth_response_ = PendingAuthResponse::none;
        pending_pk_algo_.clear();
        pending_pk_key_blob_.clear();
    }
}
