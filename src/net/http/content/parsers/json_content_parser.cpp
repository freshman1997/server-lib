#include "net/http/content/parsers/json_content_parser.h"
#include "net/http/content/types.h"
#include "net/http/content_type.h"
#include "net/http/request.h"
#include "nlohmann/json.hpp"
#include <iostream>

namespace net::http 
{
    bool JsonContentParser::can_parse(ContentType contentType)
    {
        return contentType == ContentType::application_json;
    }

    bool JsonContentParser::parse(HttpPacket *packet)
    {
        const nlohmann::json &jval = nlohmann::json::parse(packet->body_begin(), packet->body_end());
        if (jval.is_discarded()) {
            return false;
        }

        JsonContent *jc = new JsonContent;
        jc->jval = std::move(jval);
        packet->set_body_content(new Content(ContentType::application_json, jc));
        return true;
    }
}