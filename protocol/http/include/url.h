#ifndef __URL_H__
#define __URL_H__

#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::url
{
    std::string url_encode(const std::string &str);
    std::string url_decode(const std::string &str);
    std::string url_decode(const char *begin, const char *end);

    bool decode_url_domain(const std::string &url, std::vector<std::string> &urlDomain);
    bool decode_parameters(const std::string &url, std::unordered_map<std::string, std::vector<std::string>> &params, bool fromBody = false);

    struct UrlDetail
    {
        std::string protocol_;
        std::string domain_;
        std::string username_;
        std::string password_;
        std::vector<std::string> uri_;      // /aa/bb
        std::unordered_map<std::string, std::vector<std::string>> parameters_; // ?d=12#13#14&c=11
    };

    bool decode_url(const std::string &rawUrl, UrlDetail &url);

    bool encode_url(const UrlDetail &url, std::string &result);
}

#endif // !