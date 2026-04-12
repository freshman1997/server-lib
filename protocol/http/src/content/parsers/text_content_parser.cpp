#include "content/parsers/text_content_parser.h"
#include "content/types.h"
#include "packet.h"

namespace yuan::net::http
{
    bool TextContentParser::can_parse(ContentType contentType)
    {
        return contentType == ContentType::text_plain || contentType == ContentType::text_html
            || contentType == ContentType::text_javascript || contentType == ContentType::text_style_sheet;
    }

    bool TextContentParser::parse(HttpPacket *packet)
    {
        // Text parser: 将 body 数据作为纯文本存储
        auto tc = std::make_unique<TextContent>();
        tc->data.assign(packet->body_begin(), packet->body_end());
        packet->set_body_content(new Content(packet->get_content_type(), tc.release()));
        
        return true;
    }
}
