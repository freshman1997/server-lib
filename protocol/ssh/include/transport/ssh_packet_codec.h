#ifndef __NET_SSH_TRANSPORT_SSH_PACKET_CODEC_H__
#define __NET_SSH_TRANSPORT_SSH_PACKET_CODEC_H__

#include "buffer/byte_buffer.h"
#include "transport/ssh_cipher_context.h"
#include "protocol/ssh_constants.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace yuan::net::ssh
{
    class SshPacketCodec
    {
    public:
        struct ParseResult
        {
            bool complete = false;
            size_t total_bytes = 0;
        };

        static ParseResult try_parse(const ByteBuffer &buf,
                                     bool encrypted,
                                     SshCipherContext *cipher_ctx,
                                     uint32_t seq);

        static ByteBuffer encode(uint32_t seq,
                                 const uint8_t *payload, size_t len,
                                 SshCipherContext *cipher_ctx);

        static std::optional<std::vector<uint8_t> > decode(uint32_t seq,
                                                           const uint8_t *data, size_t len,
                                                           SshCipherContext *cipher_ctx);

        static size_t calculate_padding(size_t payload_len, size_t block_size);

        static constexpr size_t kMinPadding = 4;
        static constexpr size_t kMaxPadding = 255;
    };
}

#endif
