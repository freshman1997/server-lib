#ifndef __URL_H__
#define __URL_H__

#include <string>
#include <unordered_map>
#include <vector>

namespace url
{
    std::string url_encode(const std::string &str);
    std::string url_decode(const std::string &str);
    std::string url_decode(const char *begin, const char *end);

    bool decode_url_domain(const std::string &url, std::vector<std::string> &urlDomain);
    bool decode_parameters(const std::string &url, std::unordered_map<std::string, std::string> &headers, bool fromBody = false);
}

#endif // !