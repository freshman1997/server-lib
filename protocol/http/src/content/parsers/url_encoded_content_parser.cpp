#include "content/parsers/url_encoded_content_parser.h"
#include "content_type.h"
#include "url.h"
#include "packet.h"

namespace yuan::net::http
{
    bool UrlEncodedContentParser::can_parse(ContentType contentType)
    {
        return contentType == ContentType::x_www_form_urlencoded;
    }

    bool UrlEncodedContentParser::parse(HttpPacket *packet)
    {
        if (packet->get_body_content()) {
            return false;
        }
        
        const std::string &data = url::url_decode(packet->body_begin(), packet->body_end());
        return url::decode_parameters(data, packet->get_request_params(), true);
    }
}