#include "net/http/content/parsers/text_content_parser.h"
#include "net/http/content/types.h"
#include "net/http/request.h"

namespace net::http
{
    bool TextContentParser::can_parse(const content_type contentType)
    {
        return contentType == content_type::text_plain || contentType == content_type::text_html
            || contentType == content_type::text_javascript || contentType == content_type::text_style_sheet;
    }

    bool TextContentParser::parse(HttpRequest *req)
    {
        TextContent *tc = new TextContent;
        Content *content = new Content(req->get_content_type(), tc);
        tc->begin = req->body_begin();
        tc->end = req->body_end();
        req->set_body_content(content);
        return true;
    }
}
