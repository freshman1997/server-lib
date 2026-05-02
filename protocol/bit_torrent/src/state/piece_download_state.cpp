#include "state/piece_download_state.h"

#include <algorithm>

namespace yuan::net::bit_torrent
{

    void PieceDownloadState::reset(int32_t piece_count, int64_t total_length, int64_t piece_length)
    {
        const size_t count = piece_count > 0 ? static_cast<size_t>(piece_count) : 0U;
        pieces_have_.assign(count, false);
        pieces_downloading_.assign(count, false);
        next_request_offsets_.assign(count, 0);
        received_bytes_.assign(count, 0);
        retry_requests_.assign(count, {});
        inflight_requests_.assign(count, {});
        received_requests_.assign(count, {});
        total_length_ = total_length;
        piece_length_ = piece_length;
    }

    bool PieceDownloadState::is_complete() const
    {
        return !pieces_have_.empty() &&
               std::all_of(pieces_have_.begin(), pieces_have_.end(), [](bool have) { return have; });
    }

    bool PieceDownloadState::is_endgame() const
    {
        return remaining_piece_count() <= ENDGAME_THRESHOLD && remaining_piece_count() > 0;
    }

    uint32_t PieceDownloadState::remaining_piece_count() const
    {
        if (pieces_have_.empty())
            return 0;
        return static_cast<uint32_t>(std::count(pieces_have_.begin(), pieces_have_.end(), false));
    }

    void PieceDownloadState::mark_piece_downloading(uint32_t piece_index)
    {
        if (!in_range(piece_index)) {
            return;
        }

        pieces_downloading_[piece_index] = true;
    }

    uint32_t PieceDownloadState::mark_block_received(uint32_t piece_index, uint32_t offset, uint32_t length)
    {
        if (!in_range(piece_index)) {
            return 0;
        }

        const uint32_t piece_size = expected_piece_size(piece_index);
        if (piece_size == 0 || offset >= piece_size || length == 0) {
            return 0;
        }

        const uint32_t bounded_end = std::min(piece_size, offset + length);
        erase_inflight(piece_index, offset, length);
        const auto added = merge_received_interval(piece_index, offset, bounded_end - offset);
        received_bytes_[piece_index] += added;
        return added;
    }

    bool PieceDownloadState::mark_piece_completed(uint32_t piece_index)
    {
        if (!in_range(piece_index)) {
            return false;
        }

        const bool was_completed = pieces_have_[piece_index];
        pieces_have_[piece_index] = true;
        pieces_downloading_[piece_index] = false;
        next_request_offsets_[piece_index] = expected_piece_size(piece_index);
        received_bytes_[piece_index] = expected_piece_size(piece_index);
        retry_requests_[piece_index].clear();
        inflight_requests_[piece_index].clear();
        received_requests_[piece_index].clear();
        return !was_completed;
    }

    void PieceDownloadState::mark_piece_failed(uint32_t piece_index)
    {
        if (!in_range(piece_index)) {
            return;
        }

        pieces_downloading_[piece_index] = false;
        next_request_offsets_[piece_index] = 0;
        received_bytes_[piece_index] = 0;
        retry_requests_[piece_index].clear();
        inflight_requests_[piece_index].clear();
        received_requests_[piece_index].clear();
    }

    void PieceDownloadState::requeue_block(uint32_t piece_index, uint32_t offset, uint32_t length)
    {
        if (!in_range(piece_index) || pieces_have_[piece_index] || length == 0) {
            return;
        }

        const uint32_t piece_size = expected_piece_size(piece_index);
        if (piece_size == 0 || offset >= piece_size) {
            return;
        }

        PieceBlockRequest request;
        request.piece_index_ = piece_index;
        request.offset_ = offset;
        request.length_ = std::min(length, piece_size - offset);
        erase_inflight(piece_index, request.offset_, request.length_);

        auto &queue = retry_requests_[piece_index];
        const auto duplicate = std::find_if(queue.begin(), queue.end(), [&request](const PieceBlockRequest &item) {
        return item.offset_ == request.offset_ && item.length_ == request.length_;
        });
        if (duplicate == queue.end()) {
            queue.push_front(request);
        }
    }

    std::vector<PieceBlockRequest> PieceDownloadState::timeout_inflight_requests(uint64_t now_ms, uint64_t timeout_ms)
    {
        std::vector<PieceBlockRequest> timed_out;
        for (size_t i = 0; i < inflight_requests_.size(); ++i) {
            if (pieces_have_[i]) {
                continue;
            }
            auto &requests = inflight_requests_[i];
            auto it = requests.begin();
            while (it != requests.end()) {
                if (it->submit_time_ms_ != 0 && now_ms >= it->submit_time_ms_ &&
                    now_ms - it->submit_time_ms_ > timeout_ms) {
                    PieceBlockRequest req = *it;
                    req.submit_time_ms_ = 0;
                    auto &queue = retry_requests_[req.piece_index_];
                    const auto duplicate = std::find_if(queue.begin(), queue.end(),
                        [&req](const PieceBlockRequest &item) {
                            return item.offset_ == req.offset_ && item.length_ == req.length_;
                        });
                    if (duplicate == queue.end()) {
                        queue.push_front(req);
                    }
                    timed_out.push_back(*it);
                    it = requests.erase(it);
                } else {
                    ++it;
                }
            }
        }
        return timed_out;
    }

    bool PieceDownloadState::select_next_request(const std::vector<bool> & peer_pieces,
                                                 const std::vector<uint32_t> * piece_availability,
                                                 uint32_t default_request_size,
                                                 size_t max_active_pieces,
                                                 uint64_t now_ms,
                                                 PieceBlockRequest & request)
    {
        const size_t count = std::min(peer_pieces.size(), pieces_have_.size());
        std::vector<size_t> active_candidates;
        std::vector<size_t> inactive_candidates;
        active_candidates.reserve(count);
        inactive_candidates.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (!pieces_have_[i] && peer_pieces[i]) {
                if (i < pieces_downloading_.size() && pieces_downloading_[i]) {
                    active_candidates.push_back(i);
                } else {
                    inactive_candidates.push_back(i);
                }
            }
        }

        const auto sort_candidates = [piece_availability, count](std::vector<size_t> &candidates) {
        std::stable_sort(candidates.begin(), candidates.end(),
                         [piece_availability, count](size_t lhs, size_t rhs) {
                         if (piece_availability && piece_availability->size() >= count)
                         {
                             const auto left = (*piece_availability)[lhs];
                             const auto right = (*piece_availability)[rhs];
                             if (left != right)
                             {
                                 return left < right;
                             }
                         }

                         return lhs < rhs;
                     });
        };

        sort_candidates(active_candidates);
        sort_candidates(inactive_candidates);

        if (try_select_request_from_candidates(active_candidates, default_request_size, now_ms, request)) {
            return true;
        }

        if (max_active_pieces == 0) {
            max_active_pieces = 1;
        }

        if (active_piece_count() >= max_active_pieces) {
            return false;
        }

        return try_select_request_from_candidates(inactive_candidates, default_request_size, now_ms, request);
    }

    bool PieceDownloadState::select_endgame_request(const std::vector<bool> & peer_pieces,
                                                    uint64_t now_ms,
                                                    PieceBlockRequest & request)
    {
        if (!is_endgame()) {
            return false;
        }

        const size_t count = std::min(peer_pieces.size(), pieces_have_.size());
        for (size_t i = 0; i < count; ++i) {
            if (pieces_have_[i] || !peer_pieces[i]) {
                continue;
            }
            if (inflight_requests_[i].empty()) {
                continue;
            }
            request = inflight_requests_[i].front();
            request.submit_time_ms_ = now_ms;
            return true;
        }
        return false;
    }

    size_t PieceDownloadState::active_piece_count() const
    {
        return static_cast<size_t>(std::count(pieces_downloading_.begin(), pieces_downloading_.end(), true));
    }

    bool PieceDownloadState::try_select_request_from_candidates(const std::vector<size_t> & candidates,
                                                                uint32_t default_request_size,
                                                                uint64_t now_ms,
                                                                PieceBlockRequest & request)
    {
        for (const auto i : candidates) {
            const auto piece_index = static_cast<uint32_t>(i);
            const uint32_t piece_size = expected_piece_size(piece_index);
            if (piece_size == 0) {
                continue;
            }

            pieces_downloading_[i] = true;
            auto &retry_queue = retry_requests_[i];
            while (!retry_queue.empty()) {
                request = retry_queue.front();
                retry_queue.pop_front();
                if (request.offset_ < piece_size &&
                    !is_inflight(piece_index, request.offset_, request.length_)) {
                    request.length_ = std::min(request.length_, piece_size - request.offset_);
                    request.submit_time_ms_ = now_ms;
                    inflight_requests_[i].push_back(request);
                    return true;
                }
            }

            uint32_t next_offset = next_request_offsets_[i];
            while (next_offset < piece_size) {
                const uint32_t remaining = piece_size - next_offset;
                const uint32_t request_length = std::min(default_request_size, remaining);
                if (!is_inflight(piece_index, next_offset, request_length)) {
                    request.piece_index_ = piece_index;
                    request.offset_ = next_offset;
                    request.length_ = request_length;
                    request.submit_time_ms_ = now_ms;
                    next_request_offsets_[i] = next_offset + request.length_;
                    inflight_requests_[i].push_back(request);
                    return true;
                }
                next_offset += request_length;
            }
            next_request_offsets_[i] = next_offset;
        }

        return false;
    }

    bool PieceDownloadState::in_range(uint32_t piece_index) const
    {
        return piece_index < pieces_have_.size() &&
               piece_index < pieces_downloading_.size() &&
               piece_index < next_request_offsets_.size() &&
               piece_index < received_bytes_.size() &&
               piece_index < retry_requests_.size() &&
               piece_index < inflight_requests_.size() &&
               piece_index < received_requests_.size();
    }

    uint32_t PieceDownloadState::expected_piece_size(uint32_t piece_index) const
    {
        if (!in_range(piece_index) || total_length_ <= 0 || piece_length_ <= 0) {
            return 0;
        }

        const int64_t piece_offset = static_cast<int64_t>(piece_index) * piece_length_;
        if (piece_offset >= total_length_) {
            return 0;
        }

        return static_cast<uint32_t>(std::min<int64_t>(piece_length_, total_length_ - piece_offset));
    }

    bool PieceDownloadState::erase_inflight(uint32_t piece_index, uint32_t offset, uint32_t length)
    {
        if (!in_range(piece_index)) {
            return false;
        }

        auto &requests = inflight_requests_[piece_index];
        const auto it = std::find_if(requests.begin(), requests.end(),
                                     [offset, length](const PieceBlockRequest &request) {
                                     return request.offset_ == offset && request.length_ == length;
        });
        if (it == requests.end()) {
            return false;
        }

        requests.erase(it);
        return true;
    }

    bool PieceDownloadState::is_inflight(uint32_t piece_index, uint32_t offset, uint32_t length) const
    {
        if (!in_range(piece_index)) {
            return false;
        }

        const auto &requests = inflight_requests_[piece_index];
        return std::any_of(requests.begin(), requests.end(),
                           [offset, length](const PieceBlockRequest &request) {
                           return request.offset_ == offset && request.length_ == length;
        });
    }

    uint32_t PieceDownloadState::merge_received_interval(uint32_t piece_index, uint32_t offset, uint32_t length)
    {
        if (!in_range(piece_index) || length == 0) {
            return 0;
        }

        auto &requests = received_requests_[piece_index];
        uint32_t new_start = offset;
        uint32_t new_end = offset + length;
        uint32_t overlap = 0;

        for (const auto &request : requests) {
            const uint32_t existing_start = request.offset_;
            const uint32_t existing_end = request.offset_ + request.length_;
            const uint32_t overlap_start = std::max(existing_start, new_start);
            const uint32_t overlap_end = std::min(existing_end, new_end);
            if (overlap_end > overlap_start) {
                overlap += overlap_end - overlap_start;
            }

            if (existing_end < new_start || existing_start > new_end) {
                continue;
            }

            new_start = std::min(new_start, existing_start);
            new_end = std::max(new_end, existing_end);
        }

        PieceBlockRequest merged;
        merged.piece_index_ = piece_index;
        merged.offset_ = new_start;
        merged.length_ = new_end - new_start;

        requests.erase(std::remove_if(requests.begin(), requests.end(),
                                      [new_start, new_end](const PieceBlockRequest &request) {
                                      const uint32_t existing_start = request.offset_;
                                      const uint32_t existing_end = request.offset_ + request.length_;
                                      return !(existing_end < new_start || existing_start > new_end);
                       }),
                       requests.end());

        requests.push_back(merged);
        std::sort(requests.begin(), requests.end(),
                  [](const PieceBlockRequest &lhs, const PieceBlockRequest &rhs) {
                  return lhs.offset_ < rhs.offset_;
        });

        return length > overlap ? length - overlap : 0;
    }

} // namespace yuan::net::bit_torrent
