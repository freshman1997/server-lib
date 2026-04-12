#include "authorization.h"
#include "base/utils/base64.h"
#include "header_util.h"
#include <cstring>
#include <cctype>

namespace yuan::net::http
{
    std::pair<std::string, std::string> HttpAuthorization::decode_base64_credentials(const std::string &encoded)
    {
        const std::string &raw = base::util::base64_decode(encoded);
        std::size_t pos = raw.find(':');
        if (pos == std::string::npos) {
            return {raw, ""};
        }
        return {raw.substr(0, pos), raw.substr(pos + 1)};
    }

    AuthCredentials HttpAuthorization::parse(const std::string &value)
    {
        AuthCredentials creds;
        creds.raw_value = value;
        
        if (value.empty()) return creds;

        // 检查 scheme: Basic, Bearer, Digest
        auto is_prefix = [](const std::string &s, const char *prefix) -> bool {
            size_t len = std::strlen(prefix);
            if (s.size() < len) return false;
            
            for (size_t i = 0; i < len; ++i) {
                if (std::tolower(static_cast<unsigned char>(s[i])) != 
                    std::tolower(static_cast<unsigned char>(prefix[i]))) {
                    return false;
                }
            }
            return true;
        };

        if (is_prefix(value, "Basic ")) {
            creds.type = authorization_type::basic;
            auto [user, pass] = decode_base64_credentials(value.substr(6));
            creds.username = std::move(user);
            creds.password = std::move(pass);
        
        } else if (is_prefix(value, "Bearer ")) {
            creds.type = authorization_type::bearer;
            creds.token = value.substr(7);  // 跳过"Bearer "
        
        } else if (is_prefix(value, "Digest ")) {
            creds.type = authorization_type::digest;
            creds.digest_params = parse_digest_params(std::string_view(value).substr(7));
        
        } else {
            creds.type = authorization_type::unknown;
        }

        return creds;
    }

    std::pair<authorization_type, std::pair<std::string, std::string>> 
        HttpAuthorization::decode_authorization_value(const std::string &val)
    {
        if (helper::str_cmp(val.c_str(), val.c_str() + 7 , "basic; ")) {
            const auto &[user, pass] = decode_base64_credentials(val.substr(7));
            return {authorization_type::basic, {user, pass}};
        }

        // 兼容 Bearer 格式
        if (val.size() > 7 && helper::str_cmp(val.c_str(), val.c_str() + 7, "Bearer ")) {
            return {authorization_type::bearer, {"", val.substr(7)}};
        }

        return {};
    }

    bool HttpAuthorization::verify_basic(const std::string &auth_header,
                                          const std::function<bool(const std::string&, const std::string&)> &verifier)
    {
        if (!verifier || auth_header.empty()) return false;

        auto creds = parse(auth_header);
        if (creds.type != authorization_type::basic) return false;

        return verifier(creds.username, creds.password);
    }

    bool HttpAuthorization::verify_bearer(const std::string &auth_header,
                                           const std::function<bool(const std::string&)> &verifier)
    {
        if (!verifier || auth_header.empty()) return false;

        auto creds = parse(auth_header);
        if (creds.type != authorization_type::bearer) return false;

        return verifier(creds.token);
    }

    std::unordered_map<std::string, std::string> HttpAuthorization::parse_digest_params(std::string_view params)
    {
        std::unordered_map<std::string, std::string> result;
        
        size_t pos = 0;
        while (pos < params.size()) {
            // 跳过空白和逗号
            while (pos < params.size() && (params[pos] == ' ' || params[pos] == ',')) ++pos;
            
            // 读取key
            size_t key_start = pos;
            while (pos < params.size() && params[pos] != '=') ++pos;
            
            std::string key(params.substr(key_start, pos - key_start));
            if (pos >= params.size() || params[pos] != '=') break;
            ++pos; // skip '='

            // 处理引号值或非引号值
            if (pos < params.size() && params[pos] == '"') {
                ++pos; // skip opening quote
                size_t val_start = pos;
                while (pos < params.size() && params[pos] != '"') {
                    if (params[pos] == '\\' && pos + 1 < params.size()) ++pos;  // escape
                    ++pos;
                }
                result[key] = std::string(params.substr(val_start, pos - val_start));
                if (pos < params.size()) ++pos; // skip closing quote
            } else {
                size_t val_start = pos;
                while (pos < params.size() && params[pos] != ',' && params[pos] != ' ') ++pos;
                result[key] = std::string(params.substr(val_start, pos - val_start));
            }
        }
        
        return result;
    }
}
