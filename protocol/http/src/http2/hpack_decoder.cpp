#include "http2/hpack_decoder.h"
#include "http2/huffman_codec.h"

#include <array>

namespace
{
    using Field = yuan::net::http::http2::HpackHeaderField;

    constexpr std::size_t kStaticTableSize = 61;

    const std::array<Field, 62> kStaticTable = {
        Field{"", ""},
        Field{":authority", ""},
        Field{":method", "GET"},
        Field{":method", "POST"},
        Field{":path", "/"},
        Field{":path", "/index.html"},
        Field{":scheme", "http"},
        Field{":scheme", "https"},
        Field{":status", "200"},
        Field{":status", "204"},
        Field{":status", "206"},
        Field{":status", "304"},
        Field{":status", "400"},
        Field{":status", "404"},
        Field{":status", "500"},
        Field{"accept-charset", ""},
        Field{"accept-encoding", "gzip, deflate"},
        Field{"accept-language", ""},
        Field{"accept-ranges", ""},
        Field{"accept", ""},
        Field{"access-control-allow-origin", ""},
        Field{"age", ""},
        Field{"allow", ""},
        Field{"authorization", ""},
        Field{"cache-control", ""},
        Field{"content-disposition", ""},
        Field{"content-encoding", ""},
        Field{"content-language", ""},
        Field{"content-length", ""},
        Field{"content-location", ""},
        Field{"content-range", ""},
        Field{"content-type", ""},
        Field{"cookie", ""},
        Field{"date", ""},
        Field{"etag", ""},
        Field{"expect", ""},
        Field{"expires", ""},
        Field{"from", ""},
        Field{"host", ""},
        Field{"if-match", ""},
        Field{"if-modified-since", ""},
        Field{"if-none-match", ""},
        Field{"if-range", ""},
        Field{"if-unmodified-since", ""},
        Field{"last-modified", ""},
        Field{"link", ""},
        Field{"location", ""},
        Field{"max-forwards", ""},
        Field{"proxy-authenticate", ""},
        Field{"proxy-authorization", ""},
        Field{"range", ""},
        Field{"referer", ""},
        Field{"refresh", ""},
        Field{"retry-after", ""},
        Field{"server", ""},
        Field{"set-cookie", ""},
        Field{"strict-transport-security", ""},
        Field{"transfer-encoding", ""},
        Field{"user-agent", ""},
        Field{"vary", ""},
        Field{"via", ""},
        Field{"www-authenticate", ""}
    };

    bool decode_prefixed_integer(const std::vector<std::uint8_t> &block,
                                 std::size_t &offset,
                                 std::uint8_t first,
                                 std::uint8_t prefix_bits,
                                 std::size_t &out)
    {
        const std::size_t prefix_max = static_cast<std::size_t>((1u << prefix_bits) - 1u);
        out = static_cast<std::size_t>(first & static_cast<std::uint8_t>(prefix_max));
        if (out < prefix_max) {
            return true;
        }

        std::size_t shift = 0;
        while (offset < block.size()) {
            const std::uint8_t b = block[offset++];
            out += static_cast<std::size_t>(b & 0x7f) << shift;
            if ((b & 0x80) == 0) {
                return true;
            }
            shift += 7;
            if (shift > 28) {
                return false;
            }
        }
        return false;
    }

    bool decode_string_literal(const std::vector<std::uint8_t> &block,
                               std::size_t &offset,
                               std::string &out)
    {
        if (offset >= block.size()) {
            return false;
        }

        const std::uint8_t first = block[offset++];
        const bool huffman = (first & 0x80u) != 0;

        std::size_t length = 0;
        if (!decode_prefixed_integer(block, offset, first, 7, length)) {
            return false;
        }

        if (offset + length > block.size()) {
            return false;
        }

        if (huffman) {
            if (!yuan::net::http::http2::huffman_decode(&block[offset], length, out)) {
                return false;
            }
        } else {
            out.assign(reinterpret_cast<const char *>(&block[offset]), length);
        }
        offset += length;
        return true;
    }
}

namespace yuan::net::http::http2
{
    HpackDecoder::HpackDecoder() = default;

    void HpackDecoder::set_max_table_size(std::size_t size)
    {
        max_table_size_ = size;
        while (dynamic_table_size_ > max_table_size_ && !dynamic_table_.empty()) {
            dynamic_table_size_ -= dynamic_table_.back().size;
            dynamic_table_.pop_back();
        }
    }

    std::size_t HpackDecoder::max_table_size() const noexcept
    {
        return max_table_size_;
    }

    void HpackDecoder::add_to_dynamic_table(std::string_view name, std::string_view value)
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

    bool HpackDecoder::lookup_index(std::size_t index, std::string &name, std::string &value) const
    {
        if (index == 0) {
            return false;
        }

        if (index <= kStaticTableSize) {
            if (index >= kStaticTable.size()) {
                return false;
            }
            name = kStaticTable[index].name;
            value = kStaticTable[index].value;
            return true;
        }

        const std::size_t dyn_index = index - kStaticTableSize - 1;
        if (dyn_index >= dynamic_table_.size()) {
            return false;
        }

        auto it = dynamic_table_.begin();
        std::advance(it, dyn_index);
        name = it->name;
        value = it->value;
        return true;
    }

    bool HpackDecoder::decode(const std::vector<std::uint8_t> &block, std::vector<HpackHeaderField> &out_headers)
    {
        out_headers.clear();
        if (block.empty()) {
            return true;
        }

        std::size_t off = 0;
        while (off < block.size()) {
            const std::uint8_t first = block[off++];

            if ((first & 0x80u) != 0) {
                std::size_t index = 0;
                if (!decode_prefixed_integer(block, off, first, 7, index)) {
                    return false;
                }
                if (index == 0) {
                    return false;
                }
                HpackHeaderField field;
                if (!lookup_index(index, field.name, field.value)) {
                    return false;
                }
                out_headers.push_back(std::move(field));
                continue;
            }

            if ((first & 0xC0u) == 0x40u) {
                HpackHeaderField field;
                if ((first & 0x3fu) == 0) {
                    if (!decode_string_literal(block, off, field.name)) {
                        return false;
                    }
                } else {
                    std::size_t name_index = 0;
                    if (!decode_prefixed_integer(block, off, first, 6, name_index)) {
                        return false;
                    }
                    if (!lookup_index(name_index, field.name, field.value)) {
                        return false;
                    }
                    field.value.clear();
                }

                if (!decode_string_literal(block, off, field.value)) {
                    return false;
                }
                add_to_dynamic_table(field.name, field.value);
                out_headers.push_back(std::move(field));
                continue;
            }

            if ((first & 0xF0u) == 0x00u || (first & 0xF0u) == 0x10u) {
                HpackHeaderField field;
                if ((first & 0x0fu) == 0) {
                    if (!decode_string_literal(block, off, field.name)) {
                        return false;
                    }
                } else {
                    std::size_t name_index = 0;
                    if (!decode_prefixed_integer(block, off, first, 4, name_index)) {
                        return false;
                    }
                    if (!lookup_index(name_index, field.name, field.value)) {
                        return false;
                    }
                    field.value.clear();
                }

                if (!decode_string_literal(block, off, field.value)) {
                    return false;
                }
                out_headers.push_back(std::move(field));
                continue;
            }

            if ((first & 0xE0u) == 0x20u) {
                std::size_t new_max_size = 0;
                if (!decode_prefixed_integer(block, off, first, 5, new_max_size)) {
                    return false;
                }
                set_max_table_size(new_max_size);
                continue;
            }

            return false;
        }

        return true;
    }
}
