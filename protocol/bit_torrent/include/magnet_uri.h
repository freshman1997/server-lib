#ifndef __BIT_TORRENT_MAGNET_URI_H__
#define __BIT_TORRENT_MAGNET_URI_H__

#include <string>
#include <vector>
#include <cstdint>

namespace yuan::net::bit_torrent
{

    struct MagnetUri
    {
        std::vector<uint8_t> info_hash;
        std::string info_hash_hex;
        std::string display_name;
        std::vector<std::string> tracker_urls;
        bool valid = false;

        static MagnetUri parse(const std::string &uri);
    };

} // namespace yuan::net::bit_torrent

#endif
