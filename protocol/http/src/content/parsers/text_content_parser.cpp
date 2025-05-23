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
        auto preContent = packet->get_body_content();
        if (preContent && !preContent->file_info_.tmp_file_name_.empty()) {
            preContent->type_ = packet->get_content_type();
            return true;
        }

        TextContent *tc = new TextContent;
        Content *content = new Content(packet->get_content_type(), tc);
        tc->begin = packet->body_begin();
        tc->end = packet->body_end();
        packet->set_body_content(content);
        
        return true;
    }
}
