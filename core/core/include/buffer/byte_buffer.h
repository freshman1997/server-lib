#ifndef __YUAN_BUFFER_BYTE_BUFFER_H__
#define __YUAN_BUFFER_BYTE_BUFFER_H__

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "../endian/endian.hpp"

namespace yuan::buffer
{

class ByteBuffer
{
public:
    static constexpr std::size_t kDefaultCapacity = 8192;

    ByteBuffer()
        : storage_(kDefaultCapacity)
    {
    }

    explicit ByteBuffer(std::size_t capacity)
        : storage_(capacity == 0 ? kDefaultCapacity : capacity)
    {
    }

    explicit ByteBuffer(std::string_view text)
        : ByteBuffer(text.size())
    {
        append(text);
    }

    explicit ByteBuffer(std::span<const char> bytes)
        : ByteBuffer(bytes.size())
    {
        append(bytes);
    }

    ByteBuffer(const void *data, std::size_t bytes)
        : ByteBuffer(bytes)
    {
        append(data, bytes);
    }

    std::size_t capacity() const noexcept
    {
        return storage_.size();
    }

    std::size_t read_offset() const noexcept
    {
        return read_offset_;
    }

    std::size_t write_offset() const noexcept
    {
        return write_offset_;
    }

    std::size_t readable_bytes() const noexcept
    {
        return write_offset_ - read_offset_;
    }

    std::size_t writable_bytes() const noexcept
    {
        return storage_.size() - write_offset_;
    }

    bool empty() const noexcept
    {
        return readable_bytes() == 0;
    }

    char *data() noexcept
    {
        return storage_.data();
    }

    const char *data() const noexcept
    {
        return storage_.data();
    }

    char *read_ptr() noexcept
    {
        return storage_.data() + read_offset_;
    }

    const char *read_ptr() const noexcept
    {
        return storage_.data() + read_offset_;
    }

    char *write_ptr() noexcept
    {
        return storage_.data() + write_offset_;
    }

    const char *write_ptr() const noexcept
    {
        return storage_.data() + write_offset_;
    }

    std::span<const char> readable_span() const noexcept
    {
        return {read_ptr(), readable_bytes()};
    }

    std::span<char> writable_span() noexcept
    {
        return {write_ptr(), writable_bytes()};
    }

    void clear() noexcept
    {
        read_offset_ = 0;
        write_offset_ = 0;
    }

    void compact()
    {
        if (read_offset_ == 0) {
            return;
        }

        if (read_offset_ == write_offset_) {
            clear();
            return;
        }

        std::memmove(storage_.data(), read_ptr(), readable_bytes());
        write_offset_ -= read_offset_;
        read_offset_ = 0;
    }

    void reserve(std::size_t capacity)
    {
        if (capacity <= storage_.size()) {
            return;
        }

        std::vector<char> new_storage(capacity);
        const auto readable = readable_bytes();
        if (readable > 0) {
            std::memcpy(new_storage.data(), read_ptr(), readable);
        }

        storage_.swap(new_storage);
        read_offset_ = 0;
        write_offset_ = readable;
    }

    void ensure_writable(std::size_t bytes)
    {
        if (writable_bytes() >= bytes) {
            return;
        }

        if (read_offset_ > 0 && read_offset_ + writable_bytes() >= bytes) {
            compact();
            if (writable_bytes() >= bytes) {
                return;
            }
        }

        reserve(readable_bytes() + bytes);
    }

    void commit(std::size_t bytes)
    {
        write_offset_ = std::min<size_t>(write_offset_ + bytes, storage_.size());
    }

    void shrink_to_fit()
    {
        compact();
        if (storage_.size() == write_offset_) {
            return;
        }

        storage_.resize(write_offset_);
        storage_.shrink_to_fit();
    }

    void consume(std::size_t bytes) noexcept
    {
        read_offset_ = std::min<size_t>(read_offset_ + bytes, write_offset_);
    }

    void set_read_offset(std::size_t offset) noexcept
    {
        if (offset <= write_offset_) {
            read_offset_ = offset;
        }
    }

    void set_write_offset(std::size_t offset) noexcept
    {
        if (offset >= read_offset_ && offset <= storage_.size()) {
            write_offset_ = offset;
        }
    }

    void append(std::span<const char> bytes)
    {
        ensure_writable(bytes.size());
        std::memcpy(write_ptr(), bytes.data(), bytes.size());
        write_offset_ += bytes.size();
    }

    void append(const void *data, std::size_t bytes)
    {
        append(std::span<const char>(static_cast<const char *>(data), bytes));
    }

    void append(std::string_view text)
    {
        append(std::span<const char>(text.data(), text.size()));
    }

    void append(const ByteBuffer &other)
    {
        append(other.readable_span());
    }

    void append_char(char ch)
    {
        append(&ch, sizeof(ch));
    }

    void append_u8(std::uint8_t value)
    {
        append(&value, sizeof(value));
    }

    void append_i8(std::int8_t value)
    {
        append(&value, sizeof(value));
    }

    void append_u16(std::uint16_t value)
    {
        const auto network = endian::hostToNetwork16(value);
        append(&network, sizeof(network));
    }

    void append_i16(std::int16_t value)
    {
        const auto network = endian::hostToNetwork16(static_cast<std::uint16_t>(value));
        append(&network, sizeof(network));
    }

    void append_u32(std::uint32_t value)
    {
        const auto network = endian::hostToNetwork32(value);
        append(&network, sizeof(network));
    }

    void append_i32(std::int32_t value)
    {
        const auto network = endian::hostToNetwork32(static_cast<std::uint32_t>(value));
        append(&network, sizeof(network));
    }

    void append_u64(std::uint64_t value)
    {
        const auto network = endian::hostToNetwork64(value);
        append(&network, sizeof(network));
    }

    void append_i64(std::int64_t value)
    {
        const auto network = endian::hostToNetwork64(static_cast<std::uint64_t>(value));
        append(&network, sizeof(network));
    }

    std::uint8_t read_u8()
    {
        return read_trivial<std::uint8_t>();
    }

    std::int8_t read_i8()
    {
        return read_trivial<std::int8_t>();
    }

    std::uint16_t read_u16()
    {
        return endian::networkToHost16(read_trivial<std::uint16_t>());
    }

    std::int16_t read_i16()
    {
        return static_cast<std::int16_t>(endian::networkToHost16(static_cast<std::uint16_t>(read_trivial<std::int16_t>())));
    }

    std::uint32_t read_u32()
    {
        return endian::networkToHost32(read_trivial<std::uint32_t>());
    }

    std::int32_t read_i32()
    {
        return static_cast<std::int32_t>(endian::networkToHost32(static_cast<std::uint32_t>(read_trivial<std::int32_t>())));
    }

    std::uint64_t read_u64()
    {
        return endian::networkToHost64(read_trivial<std::uint64_t>());
    }

    std::int64_t read_i64()
    {
        return static_cast<std::int64_t>(endian::networkToHost64(static_cast<std::uint64_t>(read_trivial<std::int64_t>())));
    }

    ByteBuffer copy_readable() const
    {
        return ByteBuffer(readable_span());
    }

private:
    template <typename T>
    T read_trivial()
    {
        ensure_readable(sizeof(T));
        T value{};
        std::memcpy(&value, read_ptr(), sizeof(T));
        read_offset_ += sizeof(T);
        return value;
    }

    void ensure_readable(std::size_t bytes) const
    {
        if (readable_bytes() < bytes) {
            throw std::out_of_range("byte buffer underflow");
        }
    }

    std::size_t read_offset_ = 0;
    std::size_t write_offset_ = 0;
    std::vector<char> storage_;
};

} // namespace yuan::buffer

#endif
