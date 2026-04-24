#ifndef __NET_SHADOWSOCKS_SHADOWSOCKS_PACKET_CODEC_H__
#define __NET_SHADOWSOCKS_SHADOWSOCKS_PACKET_CODEC_H__

#include "buffer/byte_buffer.h"
#include "shadowsocks_protocol.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace yuan::net::shadowsocks
{
    class ShadowsocksPacketCodec
    {
    public:
        struct AeadChunkParseResult
        {
            bool complete = false;
            bool malformed = false;
            std::size_t consumed = 0;
            std::vector<uint8_t> plaintext;
        };

        struct UdpPacketParseResult
        {
            bool complete = false;
            bool malformed = false;
            std::vector<uint8_t> plaintext;
        };

        static bool append_target_address(::yuan::buffer::ByteBuffer &buf, const TargetAddress &target);

        static std::optional<TargetAddress> parse_target_address(const ::yuan::buffer::ByteBuffer &buf,
                                                                 std::size_t &consumed_bytes);

        static std::optional<TargetAddress> parse_target_address(const uint8_t *data,
                                                                 std::size_t size,
                                                                 std::size_t &consumed_bytes);

        static bool append_tcp_chunk(::yuan::buffer::ByteBuffer &out,
                                     CipherMethod method,
                                     const std::vector<uint8_t> &subkey,
                                     std::vector<uint8_t> &send_nonce,
                                     const uint8_t *payload,
                                     std::size_t payload_size);

        static AeadChunkParseResult try_parse_tcp_chunk(const uint8_t *data,
                                                        std::size_t size,
                                                        CipherMethod method,
                                                        const std::vector<uint8_t> &subkey,
                                                        std::vector<uint8_t> &recv_nonce);

        static bool append_udp_packet(::yuan::buffer::ByteBuffer &out,
                                      CipherMethod method,
                                      const std::vector<uint8_t> &master_key,
                                      const uint8_t *payload,
                                      std::size_t payload_size);

        static UdpPacketParseResult parse_udp_packet(const uint8_t *data,
                                                     std::size_t size,
                                                     CipherMethod method,
                                                     const std::vector<uint8_t> &master_key);
    };
}

#endif
