#include "net/http/content/content_parser_factory.h"
#include "net/http/content/parsers/json_content_parser.h"
#include "net/http/content/parsers/multipart_form_data_parser.h"
#include "net/http/content/parsers/text_content_parser.h"
#include "net/http/content/parsers/url_encoded_content_parser.h"
#include "net/http/content_type.h"
#include "net/http/request.h"
#include "net/http/content/content_parser.h"

namespace net::http
{
    ContentParserFactory::ContentParserFactory()
    {
        TextContentParser *textParser = new TextContentParser;
        parsers[content_type::text_plain] = textParser;
        parsers[content_type::text_html] = textParser;
        parsers[content_type::text_javascript] = textParser;
        parsers[content_type::text_style_sheet] = textParser;

        parsers[content_type::multpart_form_data] = new MultipartFormDataParser;
        parsers[content_type::application_json] = new JsonContentParser;
        parsers[content_type::x_www_form_urlencoded] = new UrlEncodedContentParser;
    }

    ContentParserFactory::~ContentParserFactory()
    {
        for (auto &item : parsers) {
            delete item.second;
        }
    }

    bool ContentParserFactory::parse_content(HttpRequest *req)
    {
        auto it = parsers.find(req->get_content_type());
        if (it == parsers.end() || !it->second->can_parse(req->get_content_type())) {
            return false;
        }

        return it->second->parse(req);
    }
}