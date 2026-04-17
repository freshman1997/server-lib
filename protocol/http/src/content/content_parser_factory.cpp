#include "content/content_parser_factory.h"
#include "content/parsers/chunked_content_parser.h"
#include "content/parsers/json_content_parser.h"
#include "content/parsers/multipart_content_parser.h"
#include "content/parsers/text_content_parser.h"
#include "content/parsers/url_encoded_content_parser.h"
#include "content_type.h"
#include "packet.h"
#include "content/content_parser.h"

namespace yuan::net::http
{
    ContentParserFactory::ContentParserFactory()
    {
        // Text parser 处理所有 text/* 类型，共享单例
        auto text = std::make_unique<TextContentParser>();
        text_parser_instance = text.get();

        parsers_[ContentType::text_plain] = text.release();
        parsers_[ContentType::text_html] = text_parser_instance;
        parsers_[ContentType::text_javascript] = text_parser_instance;
        parsers_[ContentType::text_style_sheet] = text_parser_instance;

        parsers_[ContentType::multpart_form_data] = new MultipartFormDataParser;
        parsers_[ContentType::multpart_byte_ranges] = new MultipartByterangesParser;
        parsers_[ContentType::application_json] = new JsonContentParser;
        parsers_[ContentType::x_www_form_urlencoded] = new UrlEncodedContentParser;

        // shared_text_parser 指向的对象由 parsers[text_plain] 管理
        // 析构时只 delete map 中每个 value，shared_text_parser 不单独 delete
    }

    ContentParserFactory::~ContentParserFactory()
    {
        // shared_text_parser 指向的对象由 parsers map（text_plain条目）管理
        // 避免双重释放：跳过重复引用的指针
        for (auto it = parsers_.begin(); it != parsers_.end();) {
            if (it->second == text_parser_instance && it->first != ContentType::text_plain) {
                ++it;
                continue;
            }
            delete it->second;
            it = parsers_.erase(it);
        }
    }

    bool ContentParserFactory::parse_content(HttpPacket * packet)
    {
        if (packet->is_chunked()) {
            ChunkedContentParser *chunked_parser = nullptr;

            auto *raw_parser = packet->get_pre_content_parser();
            if (!raw_parser) {
                auto owned = std::make_unique<ChunkedContentParser>();
                chunked_parser = owned.get();
                packet->set_pre_content_parser(owned.release());
            } else {
                chunked_parser = dynamic_cast<ChunkedContentParser *>(raw_parser);
                if (!chunked_parser) {
                    delete raw_parser;
                    auto owned = std::make_unique<ChunkedContentParser>();
                    chunked_parser = owned.get();
                    packet->set_pre_content_parser(owned.release());
                }
            }

            if (!chunked_parser->parse(packet)) {
                return false;
            }

            if (packet->get_body_state() == BodyState::partial) {
                return true;
            }

            packet->set_body_length(chunked_parser->get_content_length());

            chunked_parser->reset();
            delete chunked_parser;
            packet->set_pre_content_parser(nullptr);
        }

        auto it = parsers_.find(packet->get_content_type());
        if (it == parsers_.end() || !it->second->can_parse(packet->get_content_type())) {
            return true;
        }

        return it->second->parse(packet);
    }

    bool ContentParserFactory::can_parse(ContentType type)
    {
        auto it = parsers_.find(type);
        return it != parsers_.end() && it->second->can_parse(type);
    }
}