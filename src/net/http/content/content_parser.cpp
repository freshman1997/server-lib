#include <string>
#include "net/http/content/content_parser.h"

namespace net::http 
{
    bool ContentParser::can_parse(const std::string &contentType)
    {
        content_type type = find_content_type(contentType);
        return can_parse(type);
    }
}