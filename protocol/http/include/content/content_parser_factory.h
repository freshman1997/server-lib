#ifndef __NET_HTTP_CONTENT_PARSER_FACTORY_H__
#define __NET_HTTP_CONTENT_PARSER_FACTORY_H__
#include <unordered_map>
#include <memory>
#include <vector>


#include "content_type.h"
#include "singleton/singleton.h"

namespace yuan::net::http 
{
    class HttpPacket;
    class ContentParser;

    class ContentParserFactory : public singleton::Singleton<ContentParserFactory>, public std::enable_shared_from_this<ContentParserFactory>
    {
    public:
        ContentParserFactory();
        ~ContentParserFactory();

        bool parse_content(HttpPacket *packet);
        bool can_parse(ContentType type);

    private:
        std::unordered_map<ContentType, ContentParser*> parsers_;
        ContentParser* text_parser_instance = nullptr;
        std::vector<std::shared_ptr<ContentParser> > owned_parsers_;
    };
}

#endif
