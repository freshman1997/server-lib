#ifndef __NET_HTTP_AUTHORIZATION_H__
#define __NET_HTTP_AUTHORIZATION_H__

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

namespace yuan::net::http 
{
    enum class authorization_type
    {
        basic = 0,
        bearer,
        digest,
        unknown
    };

    // 解析后的认证信息
    struct AuthCredentials
    {
        authorization_type type = authorization_type::unknown;
        
        // Basic auth: username + password
        std::string username;
        std::string password;
        
        // Bearer auth: token
        std::string token;
        
        // Digest auth: raw values
        std::unordered_map<std::string, std::string> digest_params;
        
        // 原始值（用于调试）
        std::string raw_value;
    };

    class HttpAuthorization
    {
    public:
        // 解析Authorization头值，返回结构化的认证信息
        static AuthCredentials parse(const std::string &value);
        
        // 兼容旧接口
        static std::pair<authorization_type, std::pair<std::string, std::string>> decode_authorization_value(const std::string &val);
        
        // 验证Basic认证
        static bool verify_basic(const std::string &auth_header, 
                                  const std::function<bool(const std::string&, const std::string&)> &verifier);
        
        // 验证Bearer认证
        static bool verify_bearer(const std::string &auth_header,
                                   const std::function<bool(const std::string&)> &verifier);

    private:
        static std::pair<std::string, std::string> decode_base64_credentials(const std::string &encoded);
        static std::unordered_map<std::string, std::string> parse_digest_params(std::string_view params);
    };
}
#endif
