#ifndef __NET_HTTP_FORM_DATA_PARSER_H__
#define __NET_HTTP_FORM_DATA_PARSER_H__
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

#include "content/content_parser.h"

namespace yuan::net::http 
{
    class MultipartFormDataParser final : public ContentParser
    {
    public:
        bool can_parse(ContentType contentType) override;
        bool parse(HttpPacket *packet) override;

    private:
        // 解析 Content-Disposition 头部行
        // 返回 {已解析字节数, {disposition_type, {key->value参数}}}
        using DispositionResult = std::pair<uint32_t, 
            std::pair<std::string, std::unordered_map<std::string, std::string>>>;
        DispositionResult parse_content_disposition(const char *begin, const char *end);

        // 解析普通字段值（非文件）
        std::pair<uint32_t, std::string> parse_text_part(const char *begin, const char *end);

        // 解析文件部分内容
        struct FilePartResult
        {
            std::string content_type;      // 文件的 Content-Type
            std::unordered_map<std::string, std::string> content_type_params;
            uint32_t parsed_bytes = 0;     // 此part总共消耗的字节数
            const char *data_begin = nullptr;
            const char *data_end   = nullptr;
            std::string tmp_file_path;     // 如果落盘，临时文件路径；否则为空
        };
        
        int parse_file_part(FilePartResult &result, HttpPacket *packet,
                            const char *begin, const char *end, const std::string &filename);
    };

    class MultipartByterangesParser : public ContentParser
    {
    public:
        bool can_parse(ContentType contentType) override;
        bool parse(HttpPacket *packet) override;
    
    private:
        using RangeParseResult = std::tuple<bool /*ok*/, uint32_t/*from*/, uint32_t/*to*/, 
                                            uint32_t/*length*/, uint32_t/*consumed_bytes*/>;
        RangeParseResult parse_content_range(const char *begin, const char *end);
    };
}

#endif
