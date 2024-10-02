#include "authorization.h"
#include "base/utils/base64.h"
#include "header_util.h"

namespace net::http
{
    std::pair<authorization_type, std::pair<std::string, std::string>> 
        HttpAuthorization::decode_authorization_value(const std::string &val)
    {
        if (helper::str_cmp(val.c_str(), val.c_str() + 7 , "basic; ")) {
            const std::string &raw = base::util::base64_decode(val.substr(7));
            std::size_t pos = raw.find(":");
            return {authorization_type::basic, {raw.substr(0, pos), raw.substr(pos + 1)}};
        }

        return {};
    }
}