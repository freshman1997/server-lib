#include "content/content_parser.h"

namespace yuan::net::http 
{
    bool ContentParser::can_parse(const std::string &contentType)
    {
        return can_parse(find_content_type(contentType));
    }
}