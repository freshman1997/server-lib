#include "net/http/content/parsers/text_content_parser.h"

namespace net::http
{
    // 检查是否可以解析
    bool TextContentParser::can_parse(const std::string &contentType)
    {
        content_type type = find_content_type(contentType);
        return can_parse(type);
    }

    bool TextContentParser::can_parse(const content_type contentType)
    {
        return contentType == content_type::text_plain || contentType == content_type::text_html
            || contentType == content_type::text_javascript || contentType == content_type::text_style_sheet;
    }

    // 解析
    bool TextContentParser::parse(HttpRequest *req)
    {
        return true;
    }
}
