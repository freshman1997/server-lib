#ifndef YUAN_GAME_SERVER_COMMON_BINARY_CODEC_H
#define YUAN_GAME_SERVER_COMMON_BINARY_CODEC_H

#include "yuan/rpc/types.h"

#include <concepts>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <optional>
#include <vector>

namespace yuan::game::server::binary_codec
{
    class Writer;
    class Reader;

    template <typename T>
    concept BinaryMessage = requires(const T &const_value, T &value, Writer &writer, Reader &reader) {
        const_value.binary_encode(writer);
        { value.binary_decode(reader) } -> std::same_as<bool>;
    };

    class Writer
    {
    public:
        explicit Writer(yuan::rpc::Bytes &out)
            : out_(out)
        {
        }

        void clear()
        {
            out_.clear();
        }

        [[nodiscard]] bool ok() const
        {
            return ok_;
        }

        Writer &u8(std::uint8_t value)
        {
            out_.push_back(value);
            return *this;
        }

        Writer &boolean(bool value)
        {
            return u8(value ? 1 : 0);
        }

        Writer &u16(std::uint16_t value)
        {
            out_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
            out_.push_back(static_cast<std::uint8_t>(value & 0xff));
            return *this;
        }

        Writer &u32(std::uint32_t value)
        {
            out_.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
            out_.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
            out_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
            out_.push_back(static_cast<std::uint8_t>(value & 0xff));
            return *this;
        }

        Writer &u64(std::uint64_t value)
        {
            for (int shift = 56; shift >= 0; shift -= 8) {
                out_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
            }
            return *this;
        }

        Writer &string(const std::string &value)
        {
            if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
                ok_ = false;
                return *this;
            }
            u32(static_cast<std::uint32_t>(value.size()));
            const auto offset = out_.size();
            out_.resize(offset + value.size());
            if (!value.empty()) {
                std::memcpy(out_.data() + offset, value.data(), value.size());
            }
            return *this;
        }

        Writer &bytes(const yuan::rpc::Bytes &value)
        {
            if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
                ok_ = false;
                return *this;
            }
            u32(static_cast<std::uint32_t>(value.size()));
            out_.insert(out_.end(), value.begin(), value.end());
            return *this;
        }

        template <typename T, typename EncodeElement>
        Writer &vector(const std::vector<T> &values, EncodeElement encode_element)
        {
            if (values.size() > std::numeric_limits<std::uint32_t>::max()) {
                ok_ = false;
                return *this;
            }
            u32(static_cast<std::uint32_t>(values.size()));
            for (const auto &value : values) {
                encode_element(*this, value);
                if (!ok_) {
                    break;
                }
            }
            return *this;
        }

        template <typename... Values>
        Writer &fields(const Values &...values)
        {
            (field(values), ...);
            return *this;
        }

    private:
        yuan::rpc::Bytes &out_;
        bool ok_ = true;

        Writer &field(bool value) { return boolean(value); }
        Writer &field(std::uint16_t value) { return u16(value); }
        Writer &field(std::uint32_t value) { return u32(value); }
        Writer &field(std::uint64_t value) { return u64(value); }
        Writer &field(const std::string &value) { return string(value); }
        Writer &field(const yuan::rpc::Bytes &value) { return bytes(value); }

        template <typename T>
        Writer &field(const std::vector<T> &values)
        {
            return vector(values, [](Writer &writer, const T &value) { writer.field(value); });
        }

        template <typename T>
            requires requires(const T &value, Writer &writer) { value.binary_encode(writer); }
        Writer &field(const T &value)
        {
            value.binary_encode(*this);
            return *this;
        }
    };

    class Reader
    {
    public:
        explicit Reader(const yuan::rpc::Bytes &in)
            : in_(in)
        {
        }

        [[nodiscard]] std::size_t offset() const
        {
            return offset_;
        }

        void set_offset(std::size_t offset)
        {
            offset_ = offset;
        }

        [[nodiscard]] bool done() const
        {
            return offset_ == in_.size();
        }

        [[nodiscard]] bool remaining(std::size_t size) const
        {
            return in_.size() - offset_ >= size;
        }

        bool u8(std::uint8_t &value)
        {
            if (!remaining(sizeof(std::uint8_t))) {
                return false;
            }
            value = in_[offset_++];
            return true;
        }

        bool boolean(bool &value)
        {
            std::uint8_t raw = 0;
            if (!u8(raw)) {
                return false;
            }
            value = raw != 0;
            return true;
        }

        bool u16(std::uint16_t &value)
        {
            if (!remaining(sizeof(std::uint16_t))) {
                return false;
            }
            value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(in_[offset_]) << 8) | in_[offset_ + 1]);
            offset_ += sizeof(std::uint16_t);
            return true;
        }

        bool u32(std::uint32_t &value)
        {
            if (!remaining(sizeof(std::uint32_t))) {
                return false;
            }

            value = (static_cast<std::uint32_t>(in_[offset_]) << 24) |
                    (static_cast<std::uint32_t>(in_[offset_ + 1]) << 16) |
                    (static_cast<std::uint32_t>(in_[offset_ + 2]) << 8) |
                    static_cast<std::uint32_t>(in_[offset_ + 3]);
            offset_ += sizeof(std::uint32_t);

            return true;
        }

        bool u64(std::uint64_t &value)
        {
            if (!remaining(sizeof(std::uint64_t))) {
                return false;
            }

            value = 0;
            for (int i = 0; i < 8; ++i) {
                value = (value << 8) | in_[offset_ + static_cast<std::size_t>(i)];
            }
            offset_ += sizeof(std::uint64_t);

            return true;
        }

        bool string(std::string &value)
        {
            std::uint32_t size = 0;
            if (!u32(size) || !remaining(size)) {
                return false;
            }
            value.assign(reinterpret_cast<const char *>(in_.data() + offset_), size);
            offset_ += size;
            return true;
        }

        bool bytes(yuan::rpc::Bytes &value)
        {
            std::uint32_t size = 0;
            if (!u32(size) || !remaining(size)) {
                return false;
            }

            value.assign(in_.begin() + static_cast<std::ptrdiff_t>(offset_), in_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
            offset_ += size;

            return true;
        }

        template <typename T, typename DecodeElement>
        bool vector(std::vector<T> &values, DecodeElement decode_element)
        {
            std::uint32_t count = 0;
            if (!u32(count)) {
                return false;
            }

            values.resize(count);
            for (auto &value : values) {
                if (!decode_element(*this, value)) {
                    return false;
                }
            }
            
            return true;
        }

        template <typename... Values>
        bool fields(Values &...values)
        {
            return (field(values) && ...);
        }

    private:
        const yuan::rpc::Bytes &in_;
        std::size_t offset_ = 0;

        bool field(bool &value) { return boolean(value); }
        bool field(std::uint16_t &value) { return u16(value); }
        bool field(std::uint32_t &value) { return u32(value); }
        bool field(std::uint64_t &value) { return u64(value); }
        bool field(std::string &value) { return string(value); }
        bool field(yuan::rpc::Bytes &value) { return bytes(value); }

        template <typename T>
        bool field(std::vector<T> &values)
        {
            return vector(values, [](Reader &reader, T &value) { return reader.field(value); });
        }

        template <typename T>
            requires requires(T &value, Reader &reader) { { value.binary_decode(reader) } -> std::same_as<bool>; }
        bool field(T &value)
        {
            return value.binary_decode(*this);
        }
    };

    inline Writer versioned_writer(yuan::rpc::Bytes &out, std::uint32_t version = 1)
    {
        Writer writer(out);
        writer.clear();
        writer.u32(version);
        return writer;
    }

    inline bool read_version(Reader &reader, std::uint32_t expected_version = 1)
    {
        std::uint32_t version = 0;
        return reader.u32(version) && version == expected_version;
    }

    template <typename EncodeBody>
    bool write_versioned(yuan::rpc::Bytes &out, EncodeBody encode_body, std::uint32_t version = 1)
    {
        auto writer = versioned_writer(out, version);
        encode_body(writer);
        return writer.ok();
    }

    template <typename T, typename DecodeBody>
    std::optional<T> read_versioned(const yuan::rpc::Bytes &in, DecodeBody decode_body, std::uint32_t expected_version = 1)
    {
        T value;
        Reader reader(in);
        if (!read_version(reader, expected_version) || !decode_body(reader, value) || !reader.done()) {
            return std::nullopt;
        }
        return value;
    }
}

#define YUAN_GAME_BINARY_FIELDS(...)                                      \
    void binary_encode(::yuan::game::server::binary_codec::Writer &writer) const \
    {                                                                    \
        writer.fields(__VA_ARGS__);                                      \
    }                                                                    \
    bool binary_decode(::yuan::game::server::binary_codec::Reader &reader)       \
    {                                                                    \
        return reader.fields(__VA_ARGS__);                               \
    }

#endif
