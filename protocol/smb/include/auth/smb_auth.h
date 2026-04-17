#ifndef __NET_SMB_AUTH_SMB_AUTH_H__
#define __NET_SMB_AUTH_SMB_AUTH_H__

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace yuan::net::smb
{
    enum class AuthState {
        init,
        negotiate_sent,
        challenge_sent,
        authenticated,
        failed
    };

    struct AuthResult
    {
        bool success = false;
        std::vector<uint8_t> session_key;
        std::string user_name;
        std::string domain_name;
        std::vector<uint8_t> outbound_token;
    };

    class SmbAuth
    {
    public:
        virtual ~SmbAuth() = default;
        virtual AuthState state() const = 0;
        virtual std::vector<uint8_t> process_inbound_token(const std::vector<uint8_t> &token) = 0;
        virtual const AuthResult &result() const = 0;
        virtual bool is_complete() const = 0;
    };
}
#endif
