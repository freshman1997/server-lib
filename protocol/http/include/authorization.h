#ifndef __NET_HTTP_AUTHORIZATION_H__
#define __NET_HTTP_AUTHORIZATION_H__

#include <string>
#include <utility>
namespace net::http 
{
    enum class authorization_type
    {
        basic = 0,
        digest,
    };

    class HttpAuthorization
    {
    public:
        static std::pair<authorization_type, std::pair<std::string, std::string>> decode_authorization_value(const std::string &val);
    };
}

#endif