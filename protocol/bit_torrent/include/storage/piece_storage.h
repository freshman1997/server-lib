#ifndef __BIT_TORRENT_STORAGE_PIECE_STORAGE_H__
#define __BIT_TORRENT_STORAGE_PIECE_STORAGE_H__

#include "torrent_meta.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace yuan::net::bit_torrent
{

class PieceStorage
{
public:
    PieceStorage() = default;
    ~PieceStorage();

    void configure(const TorrentMeta *meta, const std::string &save_path);
    bool write_piece(int32_t piece_index, uint32_t offset, const uint8_t *data, uint32_t length);
    bool is_piece_complete(int32_t piece_index) const;
    bool verify_piece(int32_t piece_index) const;
    bool verify_committed_piece(int32_t piece_index) const;
    std::vector<uint32_t> scan_committed_pieces() const;
    std::vector<uint32_t> restore_verified_partial_pieces();
    bool commit_piece(int32_t piece_index);
    bool read_block(uint32_t piece_index, uint32_t offset, uint32_t length, std::vector<uint8_t> &out) const;
    void flush_all();
    void close_all();

private:
    int64_t expected_piece_size(int32_t piece_index) const;
    bool read_piece_data(int32_t piece_index, std::vector<uint8_t> &piece_data) const;
    bool write_piece_slice_to_file(const std::string &path,
                                   int64_t file_offset,
                                   const uint8_t *data,
                                   size_t length) const;
    bool read_piece_slice_from_file(const std::string &path,
                                    int64_t file_offset,
                                    uint8_t *data,
                                    size_t length) const;
    void close_piece_file(int32_t piece_index);
    std::string piece_file_path(int32_t piece_index) const;
    FILE *ensure_piece_file(int32_t piece_index);

private:
    const TorrentMeta *meta_ = nullptr;
    std::string save_path_;
    std::string temp_file_prefix_;
    std::unordered_map<int32_t, FILE *> piece_files_;
};

} // namespace yuan::net::bit_torrent

#endif
