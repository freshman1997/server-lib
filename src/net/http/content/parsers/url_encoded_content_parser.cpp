#include "net/http/content/parsers/url_encoded_content_parser.h"
#include "net/http/content_type.h"
#include "net/http/url.h"
#include "net/http/packet.h"

namespace net::http
{
    bool UrlEncodedContentParser::can_parse(const content_type contentType)
    {
        return contentType == content_type::x_www_form_urlencoded;
    }

    bool UrlEncodedContentParser::parse(HttpPacket *packet)
    {
        const std::string &data = url::url_decode(packet->body_begin(), packet->body_end());
        url::decode_parameters(data, packet->get_request_params(), true);
        return true;
    }
}