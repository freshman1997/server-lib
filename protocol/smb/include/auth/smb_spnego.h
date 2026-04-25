#ifndef __NET_SMB_AUTH_SMB_SPNEGO_H__
#define __NET_SMB_AUTH_SMB_SPNEGO_H__

#include "auth/smb_auth.h"
#include "auth/smb_ntlm.h"
#include <functional>
#include <memory>
#include <optional>

namespace yuan::net::smb
{
    class SmbSpnegoAuth : public SmbAuth
    {
    public:
        SmbSpnegoAuth(const std::string &server_name, const std::string &domain_name);

        AuthState state() const override;
        std::vector<uint8_t> process_inbound_token(const std::vector<uint8_t> &token) override;
        const AuthResult &result() const override;
        bool is_complete() const override;

        void set_credentials_db(std::function<bool(const std::string &, const std::string &, const std::string &)> validator);
        void set_password_lookup(std::function<std::optional<std::string>(const std::string &, const std::string &)> lookup);

    private:
        std::vector<uint8_t> extract_mech_token(const std::vector<uint8_t> &spnego_token);
        std::vector<uint8_t> wrap_ntlm_token(const std::vector<uint8_t> &ntlm_token, bool is_neg_init, bool success);

        std::unique_ptr<SmbNtlmAuth> ntlm_auth_;
    };
}
#endif
