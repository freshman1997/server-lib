#include "content/parsers/json_content_parser.h"
#include "content/types.h"
#include "content_type.h"
#include "packet.h"
#include "nlohmann/json.hpp"
#include <fstream>

namespace yuan::net::http
{
    bool JsonContentParser::can_parse(const ContentType contentType)
    {
        return contentType == ContentType::application_json;
    }

    bool JsonContentParser::parse(HttpPacket *packet)
    {
        if (const auto preContent = packet->get_body_content(); !preContent) {
            auto &reader = packet->get_buffer_reader();
            if (!reader) {
                return false;
            }

            reader.mark();
            const nlohmann::json &jval = nlohmann::json::parse(reader.begin(), reader.end());
            if (jval.is_discarded()) {
                reader.rollback();
                return false;
            }

            reader.rollback();

            auto *jc = new JsonContent;
            jc->jval = jval;
            packet->set_body_content(new Content(ContentType::application_json, jc));
        } else {
            if (!preContent->file_info_.tmp_file_name_.empty()) {
                return false;
            }

            std::ifstream file(preContent->file_info_.tmp_file_name_, std::ios::binary);
            if (!file.is_open()) {
                return false;
            }

            const nlohmann::json &jval = nlohmann::json::parse(file);
            if (jval.is_discarded()) {
                return false;
            }

            auto *jc = new JsonContent;
            jc->jval = jval;
            preContent->type_ = ContentType::application_json;
            preContent->content_data_ = jc;
        }

        return true;
    }
}