#ifndef __YUAN_BUFFER_BUFFER_CHAIN_H__
#define __YUAN_BUFFER_BUFFER_CHAIN_H__

#include <cstddef>
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
        std::size_t total = 0;
        for (const auto &buffer : buffers_) {
            if (buffer) {
                total += buffer->readable_bytes();
            }
        }
        return total;
    }

    ByteBuffer *emplace_back(std::size_t capacity = ByteBuffer::kDefaultCapacity)
    {
        buffers_.push_back(std::make_unique<ByteBuffer>(capacity));
        return &*buffers_.back();
    }

    void push_back(BufferPtr buffer)
    {
        if (buffer) {
            buffers_.push_back(std::move(buffer));
        }
    }

    BufferPtr pop_front()
    {
        if (buffers_.empty()) {
            return nullptr;
        }

        auto front = std::move(buffers_.front());
        buffers_.pop_front();
        return front;
    }

    void clear() noexcept
    {
        buffers_.clear();
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
};

} // namespace yuan::buffer

#endif
