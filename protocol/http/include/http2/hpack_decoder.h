#ifndef __NET_HTTP2_HPACK_DECODER_H__
#define __NET_HTTP2_HPACK_DECODER_H__

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace yuan::net::http::http2
{
    struct HpackHeaderField
    {
        std::string name;
        std::string value;
    };

    class HpackDecoder
    {
    public:
        HpackDecoder();

        bool decode(const std::vector<std::uint8_t> &block, std::vector<HpackHeaderField> &out_headers);
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
        bool lookup_index(std::size_t index, std::string &name, std::string &value) const;

        std::deque<DynamicEntry> dynamic_table_;
        std::size_t dynamic_table_size_ = 0;
        std::size_t max_table_size_ = 4096;
    };
}

#endif
