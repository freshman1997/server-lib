#include "net/http/content/parsers/url_encoded_content_parser.h"
#include "net/http/content_type.h"
#include "net/http/url.h"
#include "net/http/request.h"

namespace net::http
{
    bool UrlEncodedContentParser::can_parse(const content_type contentType)
    {
        return contentType == content_type::x_www_form_urlencoded;
    }

    bool UrlEncodedContentParser::parse(HttpRequest *req)
    {
        const std::string &data = url::url_decode(req->body_begin(), req->body_end());
        url::decode_parameters(data, req->get_request_params());
        return true;
    }
}