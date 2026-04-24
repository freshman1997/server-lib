#ifndef __NET_HTTP2_HPACK_ENCODER_H__
#define __NET_HTTP2_HPACK_ENCODER_H__

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace yuan::net::http::http2
{
    class HpackEncoder
    {
    public:
        HpackEncoder();

        void encode_status(std::vector<std::uint8_t> &out, std::uint16_t status);
        void encode_content_type(std::vector<std::uint8_t> &out, std::string_view content_type);
        void encode_content_length(std::vector<std::uint8_t> &out, std::size_t content_length);

        void encode_header(std::vector<std::uint8_t> &out, std::string_view name, std::string_view value);
        void set_max_table_size(std::size_t size);
        std::size_t max_table_size() const noexcept;

    private:
        struct DynamicEntry
        {
            std::string name;
            std::string value;
            std::size_t size;
        };

        void add_to_dynamic_table(std::string_view name, std::string_view value);
        std::size_t find_dynamic_table_index(std::string_view name, std::string_view value) const;
        std::size_t find_dynamic_table_name_index(std::string_view name) const;

        static void append_indexed(std::vector<std::uint8_t> &out, std::size_t index);
        static void append_literal_with_indexing(std::vector<std::uint8_t> &out,
                                                  std::size_t name_index,
                                                  std::string_view value);
        static void append_literal_with_indexing_name(std::vector<std::uint8_t> &out,
                                                       std::string_view name,
                                                       std::string_view value);
        static void append_literal_without_indexing(std::vector<std::uint8_t> &out,
                                                     std::size_t name_index,
                                                     std::string_view value);
        static void append_literal_with_name(std::vector<std::uint8_t> &out,
                                              std::string_view name,
                                              std::string_view value);
        static void append_prefixed_integer(std::vector<std::uint8_t> &out,
                                             std::uint8_t prefix_bits,
                                             std::uint8_t prefix_pattern,
                                             std::size_t value);
        static void append_string(std::vector<std::uint8_t> &out, std::string_view value);
        static void append_plain_string(std::vector<std::uint8_t> &out, std::string_view value);
        static std::size_t find_static_table_index(std::string_view name, std::string_view value);
        static std::size_t find_static_table_name_index(std::string_view name);

        std::deque<DynamicEntry> dynamic_table_;
        std::size_t dynamic_table_size_ = 0;
        std::size_t max_table_size_ = 4096;
    };
}

#endif
