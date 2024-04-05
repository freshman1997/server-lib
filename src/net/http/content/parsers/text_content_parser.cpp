#include "net/http/content/parsers/text_content_parser.h"
#include "net/http/content/types.h"
#include "net/http/packet.h"

namespace net::http
{
    bool TextContentParser::can_parse(ContentType contentType)
    {
        return contentType == ContentType::text_plain || contentType == ContentType::text_html
            || contentType == ContentType::text_javascript || contentType == ContentType::text_style_sheet;
    }

    bool TextContentParser::parse(HttpPacket *packet)
    {
        TextContent *tc = new TextContent;
        Content *content = new Content(packet->get_content_type(), tc);
        tc->begin = packet->body_begin();
        tc->end = packet->body_end();
        packet->set_body_content(content);
        return true;
    }
}
