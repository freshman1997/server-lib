#ifndef __NET_HTTP_HEADER_UTIL_H__
#define __NET_HTTP_HEADER_UTIL_H__
#include "buffer/buffer_reader.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net::http::helper
{
    enum class ContentDispositionType
    {
        unknow_ = -1,
        inline_ = 0,
        attachment_,
        form_data_,
    };

    extern std::unordered_map<std::string, ContentDispositionType> dispistion_type_mapping_;

    extern const char * dispistion_type_names[];

    extern const std::string filename_;

    extern const std::string name_;

    ContentDispositionType get_content_disposition_type(const std::string &name);

    std::string content_disposition_to_string(ContentDispositionType type);

    bool str_cmp(const char *begin, const char *end, const char *str);

    std::string read_identifier(const char *p, const char *end);

    std::string read_identifier(buffer::BufferReader &reader);

    uint32_t skip_new_line(const char *data);

    void read_next(const char *begin, const char *end, char ending, std::string &str);

    void read_next(buffer::BufferReader &reader, char ending, std::string &str);

    std::vector<std::pair<std::uint64_t, std::uint64_t>> parse_range(const std::string &range, int &ret);
};

#endif