#ifndef __YUAN_BUFFER_BUFFER_CHAIN_H__
#define __YUAN_BUFFER_BUFFER_CHAIN_H__

#include <cstddef>
#include <algorithm>
#include <functional>
#include <memory>
#include <deque>

#include "byte_buffer.h"

namespace yuan::buffer
{

class BufferChain
{
public:
    using BufferPtr = std::unique_ptr<ByteBuffer>;

    BufferChain() = default;

    ByteBuffer *front() noexcept
    {
        return buffers_.empty() ? nullptr : &*buffers_.front();
    }

    const ByteBuffer *front() const noexcept
    {
        return buffers_.empty() ? nullptr : &*buffers_.front();
    }

    ByteBuffer *back() noexcept
    {
        return buffers_.empty() ? nullptr : &*buffers_.back();
    }

    const ByteBuffer *back() const noexcept
    {
        return buffers_.empty() ? nullptr : &*buffers_.back();
    }

    bool empty() const noexcept
    {
        return buffers_.empty();
    }

    std::size_t size() const noexcept
    {
        return buffers_.size();
    }

    std::size_t readable_bytes() const noexcept
    {
        return readable_bytes_;
    }

    ByteBuffer *emplace_back(std::size_t capacity = ByteBuffer::kDefaultCapacity)
    {
        buffers_.push_back(std::make_unique<ByteBuffer>(capacity));
        return &*buffers_.back();
    }

    void push_back(BufferPtr buffer)
    {
        if (buffer) {
            readable_bytes_ += buffer->readable_bytes();
            buffers_.push_back(std::move(buffer));
        }
    }

    void push_front(BufferPtr buffer)
    {
        if (buffer) {
            readable_bytes_ += buffer->readable_bytes();
            buffers_.push_front(std::move(buffer));
        }
    }

    BufferPtr pop_front()
    {
        if (buffers_.empty()) {
            return nullptr;
        }

        auto front = std::move(buffers_.front());
        buffers_.pop_front();
        if (front) {
            const auto readable = front->readable_bytes();
            readable_bytes_ = readable_bytes_ >= readable ? readable_bytes_ - readable : 0;
        }
        return front;
    }

    void consume_front(std::size_t bytes) noexcept
    {
        auto *buffer = front();
        if (!buffer || bytes == 0) {
            return;
        }
        const auto consumed = std::min(bytes, buffer->readable_bytes());
        buffer->consume(consumed);
        readable_bytes_ = readable_bytes_ >= consumed ? readable_bytes_ - consumed : 0;
    }

    void account_append(std::size_t bytes) noexcept
    {
        readable_bytes_ += bytes;
    }

    void clear() noexcept
    {
        buffers_.clear();
        readable_bytes_ = 0;
    }

    void for_each_readable(const std::function<bool(const ByteBuffer &)> &visitor) const
    {
        for (const auto &buffer : buffers_) {
            if (!buffer || buffer->empty()) {
                continue;
            }

            if (!visitor(*buffer)) {
                break;
            }
        }
    }

private:
    std::deque<BufferPtr> buffers_;
    std::size_t readable_bytes_ = 0;
};

} // namespace yuan::buffer

#endif
