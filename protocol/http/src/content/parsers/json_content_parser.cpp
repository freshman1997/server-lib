#include "content/parsers/json_content_parser.h"
#include "content/types.h"
#include "content_type.h"
#include "packet.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include <memory>

namespace yuan::net::http 
{
    bool JsonContentParser::can_parse(const ContentType contentType)
    {
        return contentType == ContentType::application_json;
    }

    bool JsonContentParser::parse(HttpPacket *packet)
    {
        try {
            auto preContent = packet->get_body_content();
            nlohmann::json jval;

            if (!preContent) {
                // 从内存 buffer 解析
                jval = nlohmann::json::parse(packet->body_begin(), packet->body_end());
            } else {
                // 从临时文件解析（chunked 落盘场景）
                auto *chunked_data = preContent->as<ChunkedContent>();
                if (!chunked_data || chunked_data->tmp_file.empty()) {
                    return false;
                }
                std::ifstream file(chunked_data->tmp_file, std::ios::binary);
                if (!file.is_open()) {
                    return false;
                }
                jval = nlohmann::json::parse(file);
            }

            if (jval.is_discarded()) {
                return false;
            }

            // 统一使用 make_unique + release 模式，保持一致
            auto jc = std::make_unique<JsonContent>();
            jc->value = std::move(jval);

            if (preContent) {
                preContent->data.reset(static_cast<ContentData*>(jc.release()));
            } else {
                packet->set_body_content(new Content(ContentType::application_json, jc.release()));
            }

            return true;
        } catch (const nlohmann::json::exception&) {
            return false;
        } catch (const std::exception&) {
            return false;
        }
    }
}