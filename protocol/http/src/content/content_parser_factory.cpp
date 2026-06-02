#include "content/content_parser_factory.h"
#include "content/parsers/chunked_content_parser.h"
#include "content/parsers/json_content_parser.h"
#include "content/parsers/multipart_content_parser.h"
#include "content/parsers/text_content_parser.h"
#include "content/parsers/url_encoded_content_parser.h"
#include "content_type.h"
#include "packet.h"
#include "content/content_parser.h"
#include "base/owner_ptr.h"

namespace yuan::net::http
{
    namespace
    {
        template <typename T>
        T *back_ptr_of(const std::vector<std::shared_ptr<T> > &owners)
        {
            if (owners.empty()) {
                return nullptr;
            }
            return yuan::base::owner_ptr(owners.back());
        }
    }

    ContentParserFactory::ContentParserFactory()
    {
        auto text = std::make_shared<TextContentParser>();
        text_parser_instance = yuan::base::owner_ptr(text);
        owned_parsers_.push_back(text);

        parsers_[ContentType::text_plain] = text_parser_instance;
        parsers_[ContentType::text_html] = text_parser_instance;
        parsers_[ContentType::text_javascript] = text_parser_instance;
        parsers_[ContentType::text_style_sheet] = text_parser_instance;

        owned_parsers_.push_back(std::make_shared<MultipartFormDataParser>());
        parsers_[ContentType::multpart_form_data] = back_ptr_of(owned_parsers_);
        owned_parsers_.push_back(std::make_shared<MultipartByterangesParser>());
        parsers_[ContentType::multpart_byte_ranges] = back_ptr_of(owned_parsers_);
        owned_parsers_.push_back(std::make_shared<JsonContentParser>());
        parsers_[ContentType::application_json] = back_ptr_of(owned_parsers_);
        owned_parsers_.push_back(std::make_shared<UrlEncodedContentParser>());
        parsers_[ContentType::x_www_form_urlencoded] = back_ptr_of(owned_parsers_);
    }

    ContentParserFactory::~ContentParserFactory()
    {
        parsers_.clear();
        owned_parsers_.clear();
    }

    bool ContentParserFactory::parse_content(HttpPacket * packet)
    {
        if (packet->is_chunked()) {
            ChunkedContentParser *chunked_parser = nullptr;

            auto *raw_parser = packet->get_pre_content_parser();
            if (!raw_parser) {
                auto owned = std::make_shared<ChunkedContentParser>();
                chunked_parser = yuan::base::owner_ptr(owned);
                packet->set_pre_content_parser(std::move(owned));
            } else {
                chunked_parser = dynamic_cast<ChunkedContentParser *>(raw_parser);
                if (!chunked_parser) {
                    auto owned = std::make_shared<ChunkedContentParser>();
                    chunked_parser = yuan::base::owner_ptr(owned);
                    packet->set_pre_content_parser(std::move(owned));
                }
            }

            if (!chunked_parser->parse(packet)) {
                if (packet->get_body_state() == BodyState::partial) {
                    return true;
                }
                return false;
            }

            packet->set_body_length(chunked_parser->get_content_length());
            chunked_parser->reset();
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
