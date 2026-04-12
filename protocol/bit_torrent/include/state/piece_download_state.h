#ifndef __BIT_TORRENT_STATE_PIECE_DOWNLOAD_STATE_H__
#define __BIT_TORRENT_STATE_PIECE_DOWNLOAD_STATE_H__

#include <cstdint>
#include <deque>
#include <vector>

namespace yuan::net::bit_torrent
{

struct PieceBlockRequest
{
    uint32_t piece_index_ = 0;
    uint32_t offset_ = 0;
    uint32_t length_ = 0;
};

class PieceDownloadState
{
public:
    void reset(int32_t piece_count, int64_t total_length, int64_t piece_length);

    const std::vector<bool> &pieces_have() const { return pieces_have_; }
    const std::vector<bool> &pieces_downloading() const { return pieces_downloading_; }
    bool is_complete() const;

    void mark_piece_downloading(uint32_t piece_index);
    uint32_t mark_block_received(uint32_t piece_index, uint32_t offset, uint32_t length);
    bool mark_piece_completed(uint32_t piece_index);
    void mark_piece_failed(uint32_t piece_index);
    void requeue_block(uint32_t piece_index, uint32_t offset, uint32_t length);
    bool select_next_request(const std::vector<bool> &peer_pieces,
                             const std::vector<uint32_t> *piece_availability,
                             uint32_t default_request_size,
                             size_t max_active_pieces,
                             PieceBlockRequest &request);

private:
    bool in_range(uint32_t piece_index) const;
    uint32_t expected_piece_size(uint32_t piece_index) const;
    bool erase_inflight(uint32_t piece_index, uint32_t offset, uint32_t length);
    bool is_inflight(uint32_t piece_index, uint32_t offset, uint32_t length) const;
    uint32_t merge_received_interval(uint32_t piece_index, uint32_t offset, uint32_t length);
    size_t active_piece_count() const;
    bool try_select_request_from_candidates(const std::vector<size_t> &candidates,
                                            uint32_t default_request_size,
                                            PieceBlockRequest &request);

private:
    std::vector<bool> pieces_have_;
    std::vector<bool> pieces_downloading_;
    std::vector<uint32_t> next_request_offsets_;
    std::vector<uint32_t> received_bytes_;
    std::vector<std::deque<PieceBlockRequest>> retry_requests_;
    std::vector<std::vector<PieceBlockRequest>> inflight_requests_;
    std::vector<std::vector<PieceBlockRequest>> received_requests_;
    int64_t total_length_ = 0;
    int64_t piece_length_ = 0;
};

} // namespace yuan::net::bit_torrent

#endif
