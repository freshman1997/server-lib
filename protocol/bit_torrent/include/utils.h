#ifndef __BIT_TORRENT_UTILS_H__
#define __BIT_TORRENT_UTILS_H__

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace yuan::net::bit_torrent
{
    std::vector<uint8_t> sha1_hash(const unsigned char *input, std::size_t len);
    std::vector<uint8_t> sha1_hash(const std::string &data);

    std::string to_hex(const std::vector<uint8_t> &bytes);
    std::string to_hex(const uint8_t *data, size_t len);

    std::vector<uint8_t> from_hex(const std::string &hex);

    std::string url_encode(const std::string &str);

    std::string generate_peer_id();

    int64_t current_timestamp_ms();
}

#endif // __BIT_TORRENT_UTILS_H__
