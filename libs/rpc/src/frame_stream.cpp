#include "yuan/rpc/frame_stream.h"

#include <utility>

namespace yuan::rpc::wire
{
    FrameStreamDecoder::FrameStreamDecoder(DecodeOptions options)
        : options_(std::move(options))
    {
    }

    void FrameStreamDecoder::append(const std::uint8_t *data, std::size_t size)
    {
        buffer_.insert(buffer_.end(), data, data + size);
    }

    void FrameStreamDecoder::append(const Bytes &bytes)
    {
        append(bytes.data(), bytes.size());
    }

    DecodeResult FrameStreamDecoder::next()
    {
        auto result = decode_frame(buffer_.data(), buffer_.size(), options_);
        if (result.ok) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(result.consumed));
        }
        return result;
    }

    std::size_t FrameStreamDecoder::buffered_size() const
    {
        return buffer_.size();
    }

    void FrameStreamDecoder::clear()
    {
        buffer_.clear();
    }
}
