#ifndef __YUAN_BUFFER_BYTE_BUFFER_READER_H__
#define __YUAN_BUFFER_BYTE_BUFFER_READER_H__

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

#include "byte_buffer.h"
#include "buffer_chain.h"

namespace yuan::buffer
{

    class ByteBufferReader
    {
    public:
        struct Position
        {
            std::size_t buffer_index = 0;
            std::size_t buffer_offset = 0;
            std::size_t offset = 0;
        };

        ByteBufferReader() = default;

        explicit ByteBufferReader(const ByteBuffer *buffer)
        {
            add_buffer(buffer);
        }

        explicit ByteBufferReader(const BufferChain &chain)
        {
            chain.for_each_readable([this](const ByteBuffer &buffer) {
            add_buffer(&buffer);
            return true;
            });
        }

        void reset() noexcept
        {
            buffers_.clear();
            owned_buffers_.clear();
            current_buffer_index_ = 0;
            current_buffer_offset_ = 0;
            current_offset_ = 0;
            total_bytes_ = 0;
            mark_buffer_index_ = 0;
            mark_buffer_offset_ = 0;
            mark_offset_ = 0;
        }

        void init() noexcept
        {
            reset();
        }

        void add_buffer(const ByteBuffer *buffer)
        {
            if (!buffer || buffer->empty()) {
                return;
            }

            buffers_.push_back(buffer);
            total_bytes_ += buffer->readable_bytes();
        }

        void add_buffer(const ByteBuffer &buffer)
        {
            if (buffer.empty()) {
                return;
            }

            owned_buffers_.push_back(buffer);
            add_buffer(&owned_buffers_.back());
        }

        void mark() noexcept
        {
            mark_buffer_index_ = current_buffer_index_;
            mark_buffer_offset_ = current_buffer_offset_;
            mark_offset_ = current_offset_;
        }

        void rollback() noexcept
        {
            current_buffer_index_ = mark_buffer_index_;
            current_buffer_offset_ = mark_buffer_offset_;
            current_offset_ = mark_offset_;
        }

        Position position() const noexcept
        {
            return Position{ current_buffer_index_, current_buffer_offset_, current_offset_ };
        }

        void restore(Position position) noexcept
        {
            current_buffer_index_ = position.buffer_index;
            current_buffer_offset_ = position.buffer_offset;
            current_offset_ = position.offset;
        }

        std::size_t remaining_bytes() const noexcept
        {
            return total_bytes_ - current_offset_;
        }

        std::size_t get_remain_bytes() const noexcept
        {
            return remaining_bytes();
        }

        bool empty() const noexcept
        {
            return remaining_bytes() == 0;
        }

        char peek_char() const noexcept
        {
            const auto *buffer = current_buffer();
            if (!buffer) {
                return 0;
            }

            return buffer->read_ptr()[current_buffer_offset_];
        }

        char read_char() noexcept
        {
            const char ch = peek_char();
            advance(1);
            return ch;
        }

        int read(char *dest, std::size_t bytes)
        {
            if (!dest || bytes == 0) {
                return 0;
            }

            bytes = std::min<size_t>(bytes, remaining_bytes());
            std::size_t copied = 0;
            while (copied < bytes) {
                const auto *buffer = current_buffer();
                if (!buffer) {
                    break;
                }

                const auto available = buffer->readable_bytes() - current_buffer_offset_;
                const auto chunk = std::min<std::size_t>(available, bytes - copied);
                std::memcpy(dest + copied, buffer->read_ptr() + current_buffer_offset_, chunk);
                copied += chunk;
                advance(chunk);
            }

            return static_cast<int>(copied);
        }

        int read_line(std::string &line)
        {
            bool completed = false;
            while (remaining_bytes() > 0) {
                const char ch = peek_char();
                if (ch == '\n') {
                    read_char();
                    completed = true;
                    break;
                }

                if (ch == '\r') {
                    read_char();
                    if (remaining_bytes() == 0) {
                        return -2;
                    }
                    if (peek_char() == '\n') {
                        read_char();
                        completed = true;
                        break;
                    }
                    return -1;
                }

                line.push_back(ch);
                read_char();
            }

            return completed ? 0 : -2;
        }

        std::uint64_t write(std::ofstream &out)
        {
            if (!out.is_open() || !out.good()) {
                return static_cast<std::uint64_t>(-1);
            }

            std::uint64_t written = 0;
            while (remaining_bytes() > 0) {
                const auto *buffer = current_buffer();
                if (!buffer) {
                    break;
                }

                const auto available = buffer->readable_bytes() - current_buffer_offset_;
                out.write(buffer->read_ptr() + current_buffer_offset_, static_cast<std::streamsize>(available));
                if (out.fail()) {
                    return static_cast<std::uint64_t>(-1);
                }

                written += available;
                advance(available);
            }

            return written;
        }

        void just_clear() noexcept
        {
            reset();
        }

        void discard_read_bytes()
        {
            if (current_offset_ == 0) {
                return;
            }

            if (current_offset_ >= total_bytes_) {
                reset();
                return;
            }

            const auto bytes = remaining_bytes();
            std::vector<char> remaining(bytes);
            read(remaining.data(), bytes);

            reset();
            add_buffer(ByteBuffer(std::span<const char>(remaining.data(), remaining.size())));
        }

    private:
        const ByteBuffer *current_buffer() const noexcept
        {
            if (current_buffer_index_ >= buffers_.size()) {
                return nullptr;
            }
            return buffers_[current_buffer_index_];
        }

        void advance(std::size_t bytes) noexcept
        {
            bytes = std::min<size_t>(bytes, remaining_bytes());
            current_offset_ += bytes;

            while (bytes > 0 && current_buffer_index_ < buffers_.size()) {
                const auto *buffer = buffers_[current_buffer_index_];
                const auto available = buffer->readable_bytes() - current_buffer_offset_;
                if (bytes < available) {
                    current_buffer_offset_ += bytes;
                    return;
                }

                bytes -= available;
                ++current_buffer_index_;
                current_buffer_offset_ = 0;
            }
        }

        std::vector<const ByteBuffer *> buffers_;
        std::deque<ByteBuffer> owned_buffers_;
        std::size_t current_buffer_index_ = 0;
        std::size_t current_buffer_offset_ = 0;
        std::size_t current_offset_ = 0;
        std::size_t total_bytes_ = 0;
        std::size_t mark_buffer_index_ = 0;
        std::size_t mark_buffer_offset_ = 0;
        std::size_t mark_offset_ = 0;
    };

} // namespace yuan::buffer

#endif
