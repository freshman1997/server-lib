#include "http2/hpack_encoder.h"
#include "http2/huffman_codec.h"

#include <array>
#include <cstring>
#include <string>

namespace yuan::net::http::http2
{
    namespace
    {
        struct StaticTableEntry
        {
            std::size_t index;
            const char *name;
            const char *value;
        };

        constexpr std::array<StaticTableEntry, 61> kStaticTable = {
            StaticTableEntry{ 1, ":authority", "" },
            StaticTableEntry{ 2, ":method", "GET" },
            StaticTableEntry{ 3, ":method", "POST" },
            StaticTableEntry{ 4, ":path", "/" },
            StaticTableEntry{ 5, ":path", "/index.html" },
            StaticTableEntry{ 6, ":scheme", "http" },
            StaticTableEntry{ 7, ":scheme", "https" },
            StaticTableEntry{ 8, ":status", "200" },
            StaticTableEntry{ 9, ":status", "204" },
            StaticTableEntry{ 10, ":status", "206" },
            StaticTableEntry{ 11, ":status", "304" },
            StaticTableEntry{ 12, ":status", "400" },
            StaticTableEntry{ 13, ":status", "404" },
            StaticTableEntry{ 14, ":status", "500" },
            StaticTableEntry{ 15, "accept-charset", "" },
            StaticTableEntry{ 16, "accept-encoding", "gzip, deflate" },
            StaticTableEntry{ 17, "accept-language", "" },
            StaticTableEntry{ 18, "accept-ranges", "" },
            StaticTableEntry{ 19, "accept", "" },
            StaticTableEntry{ 20, "access-control-allow-origin", "" },
            StaticTableEntry{ 21, "age", "" },
            StaticTableEntry{ 22, "allow", "" },
            StaticTableEntry{ 23, "authorization", "" },
            StaticTableEntry{ 24, "cache-control", "" },
            StaticTableEntry{ 25, "content-disposition", "" },
            StaticTableEntry{ 26, "content-encoding", "" },
            StaticTableEntry{ 27, "content-language", "" },
            StaticTableEntry{ 28, "content-length", "" },
            StaticTableEntry{ 29, "content-location", "" },
            StaticTableEntry{ 30, "content-range", "" },
            StaticTableEntry{ 31, "content-type", "" },
            StaticTableEntry{ 32, "cookie", "" },
            StaticTableEntry{ 33, "date", "" },
            StaticTableEntry{ 34, "etag", "" },
            StaticTableEntry{ 35, "expect", "" },
            StaticTableEntry{ 36, "expires", "" },
            StaticTableEntry{ 37, "from", "" },
            StaticTableEntry{ 38, "host", "" },
            StaticTableEntry{ 39, "if-match", "" },
            StaticTableEntry{ 40, "if-modified-since", "" },
            StaticTableEntry{ 41, "if-none-match", "" },
            StaticTableEntry{ 42, "if-range", "" },
            StaticTableEntry{ 43, "if-unmodified-since", "" },
            StaticTableEntry{ 44, "last-modified", "" },
            StaticTableEntry{ 45, "link", "" },
            StaticTableEntry{ 46, "location", "" },
            StaticTableEntry{ 47, "max-forwards", "" },
            StaticTableEntry{ 48, "proxy-authenticate", "" },
            StaticTableEntry{ 49, "proxy-authorization", "" },
            StaticTableEntry{ 50, "range", "" },
            StaticTableEntry{ 51, "referer", "" },
            StaticTableEntry{ 52, "refresh", "" },
            StaticTableEntry{ 53, "retry-after", "" },
            StaticTableEntry{ 54, "server", "" },
            StaticTableEntry{ 55, "set-cookie", "" },
            StaticTableEntry{ 56, "strict-transport-security", "" },
            StaticTableEntry{ 57, "transfer-encoding", "" },
            StaticTableEntry{ 58, "user-agent", "" },
            StaticTableEntry{ 59, "vary", "" },
            StaticTableEntry{ 60, "via", "" },
            StaticTableEntry{ 61, "www-authenticate", "" }
        };

        constexpr std::size_t kStaticTableSize = 61;

        bool str_eq_case_insensitive(std::string_view a, const char *b)
        {
            const std::size_t len = std::strlen(b);
            if (a.size() != len) {
                return false;
            }
            for (std::size_t i = 0; i < len; ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
                    return false;
                }
            }
            return true;
        }

        bool str_eq_case_insensitive_sv(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size()) {
                return false;
            }
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
                    return false;
                }
            }
            return true;
        }
    }

    HpackEncoder::HpackEncoder() = default;

    void HpackEncoder::set_max_table_size(std::size_t size)
    {
        max_table_size_ = size;
        while (dynamic_table_size_ > max_table_size_ && !dynamic_table_.empty()) {
            dynamic_table_size_ -= dynamic_table_.back().size;
            dynamic_table_.pop_back();
        }
    }

    std::size_t HpackEncoder::max_table_size() const noexcept
    {
        return max_table_size_;
    }

    void HpackEncoder::add_to_dynamic_table(std::string_view name, std::string_view value)
    {
        const std::size_t entry_size = 32 + name.size() + value.size();
        if (entry_size > max_table_size_) {
            while (!dynamic_table_.empty()) {
                dynamic_table_size_ -= dynamic_table_.back().size;
                dynamic_table_.pop_back();
            }
            return;
        }

        while (dynamic_table_size_ + entry_size > max_table_size_ && !dynamic_table_.empty()) {
            dynamic_table_size_ -= dynamic_table_.back().size;
            dynamic_table_.pop_back();
        }

        DynamicEntry entry;
        entry.name = std::string(name);
        entry.value = std::string(value);
        entry.size = entry_size;
        dynamic_table_.push_front(std::move(entry));
        dynamic_table_size_ += entry_size;
    }

    std::size_t HpackEncoder::find_dynamic_table_index(std::string_view name, std::string_view value) const
    {
        std::size_t idx = 0;
        for (const auto &entry : dynamic_table_) {
            if (str_eq_case_insensitive_sv(name, entry.name) && value == entry.value) {
                return kStaticTableSize + 1 + idx;
            }
            ++idx;
        }
        return 0;
    }

    std::size_t HpackEncoder::find_dynamic_table_name_index(std::string_view name) const
    {
        std::size_t idx = 0;
        for (const auto &entry : dynamic_table_) {
            if (str_eq_case_insensitive_sv(name, entry.name)) {
                return kStaticTableSize + 1 + idx;
            }
            ++idx;
        }
        return 0;
    }

    void HpackEncoder::append_prefixed_integer(std::vector<std::uint8_t> &out,
                                               std::uint8_t prefix_bits,
                                               std::uint8_t prefix_pattern,
                                               std::size_t value)
    {
        const std::size_t prefix_max = (static_cast<std::size_t>(1u) << prefix_bits) - 1u;
        if (value < prefix_max) {
            out.push_back(static_cast<std::uint8_t>(prefix_pattern | static_cast<std::uint8_t>(value)));
            return;
        }

        out.push_back(static_cast<std::uint8_t>(prefix_pattern | static_cast<std::uint8_t>(prefix_max)));
        value -= prefix_max;
        while (value >= 128) {
            out.push_back(static_cast<std::uint8_t>((value % 128) + 128));
            value /= 128;
        }
        out.push_back(static_cast<std::uint8_t>(value));
    }

    void HpackEncoder::append_plain_string(std::vector<std::uint8_t> &out, std::string_view value)
    {
        append_prefixed_integer(out, 7, 0x00, value.size());
        out.insert(out.end(), value.begin(), value.end());
    }

    void HpackEncoder::append_string(std::vector<std::uint8_t> &out, std::string_view value)
    {
        if (value.size() < 6) {
            append_plain_string(out, value);
            return;
        }

        std::vector<std::uint8_t> huffed;
        huffed.reserve(value.size());
        huffman_encode(value, huffed);

        if (huffed.size() < value.size()) {
            append_prefixed_integer(out, 7, 0x80, huffed.size());
            out.insert(out.end(), huffed.begin(), huffed.end());
        } else {
            append_plain_string(out, value);
        }
    }

    void HpackEncoder::append_indexed(std::vector<std::uint8_t> &out, std::size_t index)
    {
        append_prefixed_integer(out, 7, 0x80, index);
    }

    void HpackEncoder::append_literal_without_indexing(std::vector<std::uint8_t> &out,
                                                        std::size_t name_index,
                                                        std::string_view value)
    {
        append_prefixed_integer(out, 4, 0x00, name_index);
        append_string(out, value);
    }

    void HpackEncoder::append_literal_with_indexing(std::vector<std::uint8_t> &out,
                                                     std::size_t name_index,
                                                     std::string_view value)
    {
        append_prefixed_integer(out, 6, 0x40, name_index);
        append_string(out, value);
    }

    void HpackEncoder::append_literal_with_indexing_name(std::vector<std::uint8_t> &out,
                                                          std::string_view name,
                                                          std::string_view value)
    {
        out.push_back(0x40);
        append_string(out, name);
        append_string(out, value);
    }

    void HpackEncoder::append_literal_with_name(std::vector<std::uint8_t> &out,
                                                 std::string_view name,
                                                 std::string_view value)
    {
        out.push_back(0x00);
        append_string(out, name);
        append_string(out, value);
    }

    std::size_t HpackEncoder::find_static_table_index(std::string_view name, std::string_view value)
    {
        for (const auto &entry : kStaticTable) {
            if (str_eq_case_insensitive(name, entry.name) && value == entry.value && entry.value[0] != '\0') {
                return entry.index;
            }
        }
        return 0;
    }

    std::size_t HpackEncoder::find_static_table_name_index(std::string_view name)
    {
        for (const auto &entry : kStaticTable) {
            if (str_eq_case_insensitive(name, entry.name)) {
                return entry.index;
            }
        }
        return 0;
    }

    void HpackEncoder::encode_header(std::vector<std::uint8_t> &out, std::string_view name, std::string_view value)
    {
        const std::size_t static_full = find_static_table_index(name, value);
        if (static_full != 0) {
            append_indexed(out, static_full);
            return;
        }

        const std::size_t dynamic_full = find_dynamic_table_index(name, value);
        if (dynamic_full != 0) {
            append_indexed(out, dynamic_full);
            return;
        }

        const std::size_t should_index = name.size() + value.size() + 32;
        if (should_index <= max_table_size_) {
            const std::size_t static_name = find_static_table_name_index(name);
            if (static_name != 0) {
                append_literal_with_indexing(out, static_name, value);
            } else {
                const std::size_t dynamic_name = find_dynamic_table_name_index(name);
                if (dynamic_name != 0) {
                    append_literal_with_indexing(out, dynamic_name, value);
                } else {
                    append_literal_with_indexing_name(out, name, value);
                }
            }
            add_to_dynamic_table(name, value);
            return;
        }

        const std::size_t name_index = find_static_table_name_index(name);
        if (name_index != 0) {
            append_literal_without_indexing(out, name_index, value);
        } else {
            append_literal_with_name(out, name, value);
        }
    }

    void HpackEncoder::encode_status(std::vector<std::uint8_t> &out, std::uint16_t status)
    {
        const std::string status_str = std::to_string(status);
        encode_header(out, ":status", status_str);
    }

    void HpackEncoder::encode_content_type(std::vector<std::uint8_t> &out, std::string_view content_type)
    {
        encode_header(out, "content-type", content_type);
    }

    void HpackEncoder::encode_content_length(std::vector<std::uint8_t> &out, std::size_t content_length)
    {
        encode_header(out, "content-length", std::to_string(content_length));
    }
}
