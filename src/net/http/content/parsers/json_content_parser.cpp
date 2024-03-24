#include "net/http/content/parsers/json_content_parser.h"
#include "net/http/content/types.h"
#include "net/http/content_type.h"
#include "net/http/request.h"
#include "nlohmann/json.hpp"
#include <iostream>

namespace net::http 
{
    bool JsonContentParser::can_parse(const content_type contentType)
    {
        return contentType == content_type::application_json;
    }

    bool JsonContentParser::parse(HttpRequest *req)
    {
        const nlohmann::json &jval = nlohmann::json::parse(req->body_begin(), req->body_end());
        if (jval.is_discarded()) {
            return false;
        }

        JsonContent *jc = new JsonContent;
        jc->jval = std::move(jval);
        std::cout << jc->jval << std::endl;
        Content *content = new Content(content_type::application_json, jc);
        req->set_body_content(content);
        return true;
    }
}