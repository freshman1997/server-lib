#ifndef __BIT_TORRENT_TORRENT_META_H__
#define __BIT_TORRENT_TORRENT_META_H__

#include "structure/bencoding.h"
#include <string>
#include <vector>
#include <cstdint>

namespace yuan::net::bit_torrent
{

struct TorrentFile
{
    int64_t length_ = 0;
    std::string md5sum_;
    std::vector<std::string> path_;  // multi-file mode
    int64_t offset_ = 0;             // offset within the full torrent
};

struct PeerAddress
{
    std::string ip_;
    uint16_t port_ = 0;
};

struct TorrentInfo
{
    int64_t piece_length_ = 0;
    std::string pieces_;              // concatenated 20-byte SHA-1 hashes
    bool private_ = false;
    std::string name_;

    int64_t total_length_ = 0;        // single-file: length, multi-file: sum
    std::vector<TorrentFile> files_;

    int32_t piece_count() const
    {
        if (piece_length_ <= 0) return 0;
        return static_cast<int32_t>((total_length_ + piece_length_ - 1) / piece_length_);
    }

    std::string piece_hash(int32_t index) const
    {
        if (index < 0 || index >= piece_count()) return "";
        return pieces_.substr(index * 20, 20);
    }
};

struct TorrentMeta
{
    std::string announce_;
    std::vector<std::vector<std::string>> announce_list_;
    TorrentInfo info;
    std::string comment_;
    std::string created_by_;
    int64_t creation_date_ = 0;
    int32_t encoding_ = 0;  // 0=utf8, 1=gbk

    std::string info_hash_hex_;
    std::vector<uint8_t> info_hash_;  // 20-byte SHA-1 of bencoded info dict

    static TorrentMeta parse(const std::string &torrent_data);
    static TorrentMeta parse_file(const std::string &file_path);
};

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_TORRENT_META_H__
