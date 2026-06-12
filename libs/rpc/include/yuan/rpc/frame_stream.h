#ifndef YUAN_RPC_FRAME_STREAM_H
#define YUAN_RPC_FRAME_STREAM_H

#include "wire.h"

namespace yuan::rpc::wire
{
    class FrameStreamDecoder
    {
    public:
        explicit FrameStreamDecoder(DecodeOptions options = {});

        void append(const std::uint8_t *data, std::size_t size);

        void append(const Bytes &bytes);

        DecodeResult next();

        [[nodiscard]] std::size_t buffered_size() const;

        void clear();

    private:
        DecodeOptions options_;
        Bytes buffer_;
    };
}

#endif
