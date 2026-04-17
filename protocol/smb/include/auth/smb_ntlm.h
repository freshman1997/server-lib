#ifndef __NET_SMB_AUTH_SMB_NTLM_H__
#define __NET_SMB_AUTH_SMB_NTLM_H__

#include "auth/smb_auth.h"
#include "protocol/smb2_constants.h"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace yuan::net::smb
{
    struct NtlmType1Message
    {
        uint32_t flags = 0;
        std::string domain_name;
        std::string workstation;
        std::vector<uint8_t> raw;
    };

    struct NtlmType2Message
    {
        std::array<uint8_t, 8> server_challenge{};
        std::vector<uint8_t> target_info;
        uint32_t flags = 0;
        std::string target_name;
    };

    struct NtlmType3Message
    {
        std::string user_name;
        std::string domain_name;
        std::string workstation;
        std::vector<uint8_t> lm_response;
        std::vector<uint8_t> ntlm_response;
        uint32_t flags = 0;
    };

    class SmbNtlmAuth : public SmbAuth
    {
    public:
        explicit SmbNtlmAuth(const std::string &server_name, const std::string &domain_name);

        AuthState state() const override
        {
            return state_;
        }
        std::vector<uint8_t> process_inbound_token(const std::vector<uint8_t> &token) override;
        const AuthResult &result() const override
        {
            return result_;
        }
        bool is_complete() const override;

        void set_credentials_db(std::function<bool(const std::string &, const std::string &, const std::string &)> validator);

        static std::optional<NtlmType1Message> parse_type1(const uint8_t *data, size_t len);
        static std::optional<NtlmType3Message> parse_type3(const uint8_t *data, size_t len);
        static std::vector<uint8_t> build_type2(const std::array<uint8_t, 8> &server_challenge,
                                                const std::string &target_name,
                                                const std::vector<uint8_t> &target_info,
                                                uint32_t flags);
        static std::vector<uint8_t> ntlmv2_response(const std::string &password,
                                                    const std::string &username,
                                                    const std::string &domain,
                                                    const std::array<uint8_t, 8> &server_challenge,
                                                    const std::vector<uint8_t> &target_info);
        static std::vector<uint8_t> ntlmowfv1(const std::string &password);
        static std::vector<uint8_t> ntlmowfv2(const std::string &password, const std::string &username, const std::string &domain);
        static std::vector<uint8_t> hmac_md5(const std::vector<uint8_t> &key, const std::vector<uint8_t> &data);
        static std::vector<uint8_t> md4(const std::vector<uint8_t> &data);
        static std::vector<uint8_t> md5(const std::vector<uint8_t> &data);
        static std::array<uint8_t, 8> generate_server_challenge();

    private:
        std::string server_name_;
        std::string domain_name_;
        AuthState state_ = AuthState::init;
        AuthResult result_;
        std::array<uint8_t, 8> server_challenge_{};
        uint32_t negotiate_flags_ = 0;
        std::function<bool(const std::string &, const std::string &, const std::string &)> credential_validator_;
    };
}
#endif
