#include "net/http/content/content_parser_factory.h"
#include "net/http/content/parsers/json_content_parser.h"
#include "net/http/content/parsers/multipart_content_parser.h"
#include "net/http/content/parsers/text_content_parser.h"
#include "net/http/content/parsers/url_encoded_content_parser.h"
#include "net/http/content_type.h"
#include "net/http/packet.h"
#include "net/http/content/content_parser.h"

namespace net::http
{
    ContentParserFactory::ContentParserFactory()
    {
        parsers[ContentType::text_plain] = new TextContentParser;
        parsers[ContentType::text_html] = new TextContentParser;
        parsers[ContentType::text_javascript] = new TextContentParser;
        parsers[ContentType::text_style_sheet] = new TextContentParser;

        parsers[ContentType::multpart_form_data] = new MultipartFormDataParser;
        parsers[ContentType::multpart_byte_ranges] = new MultipartByterangesParser;
        parsers[ContentType::application_json] = new JsonContentParser;
        parsers[ContentType::x_www_form_urlencoded] = new UrlEncodedContentParser;
    }

    ContentParserFactory::~ContentParserFactory()
    {
        for (auto &item : parsers) {
            delete item.second;
        }
    }

    bool ContentParserFactory::parse_content(HttpPacket *packet)
    {
        auto it = parsers.find(packet->get_content_type());
        if (it == parsers.end() || !it->second->can_parse(packet->get_content_type())) {
            return false;
        }

        return it->second->parse(packet);
    }
}