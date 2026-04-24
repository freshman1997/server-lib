#ifndef __NET_HTTP2_HUFFMAN_CODEC_H__
#define __NET_HTTP2_HUFFMAN_CODEC_H__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::http::http2
{
    bool huffman_decode(const std::uint8_t *data, std::size_t len, std::string &out);
    void huffman_encode(const std::uint8_t *data, std::size_t len, std::vector<std::uint8_t> &out);
    void huffman_encode(std::string_view input, std::vector<std::uint8_t> &out);
}
#endif
