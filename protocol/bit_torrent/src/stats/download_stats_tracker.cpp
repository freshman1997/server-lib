#include "stats/download_stats_tracker.h"

#include <algorithm>

namespace yuan::net::bit_torrent
{

void DownloadStatsTracker::reset(int64_t total_bytes, int32_t total_pieces)
{
    stats_ = {};
    stats_.total_bytes_ = total_bytes;
    stats_.total_pieces_ = total_pieces;
    completed_pieces_.assign(static_cast<size_t>(std::max(total_pieces, 0)), false);
    update_progress();
}

void DownloadStatsTracker::add_downloaded_bytes(uint32_t bytes)
{
    stats_.downloaded_bytes_ += bytes;
}

void DownloadStatsTracker::add_uploaded_bytes(uint32_t bytes)
{
    stats_.uploaded_bytes_ += bytes;
}

void DownloadStatsTracker::set_piece_completed(uint32_t piece_index)
{
    if (piece_index >= completed_pieces_.size())
    {
        return;
    }

    if (completed_pieces_[piece_index])
    {
        return;
    }

    completed_pieces_[piece_index] = true;
    ++stats_.pieces_downloaded_;
    update_progress();
}

void DownloadStatsTracker::update_peer_counts(int32_t active_peers, int32_t total_peers)
{
    stats_.active_peers_ = active_peers;
    stats_.total_peers_ = total_peers;
}

TrackerAnnounceContext DownloadStatsTracker::make_tracker_context(const TorrentMeta &meta, int32_t listen_port) const
{
    TrackerAnnounceContext ctx;
    ctx.meta_ = &meta;
    ctx.listen_port_ = listen_port;
    ctx.uploaded_ = stats_.uploaded_bytes_;
    ctx.downloaded_ = stats_.downloaded_bytes_;
    ctx.left_ = std::max<int64_t>(0, meta.info.total_length_ - stats_.downloaded_bytes_);
    return ctx;
}

void DownloadStatsTracker::emit()
{
    if (callback_)
    {
        callback_(stats_);
    }
}

void DownloadStatsTracker::update_progress()
{
    if (stats_.total_pieces_ <= 0)
    {
        stats_.progress_ = 0.0f;
        return;
    }

    stats_.progress_ = static_cast<float>(stats_.pieces_downloaded_) /
                       static_cast<float>(stats_.total_pieces_);
}

} // namespace yuan::net::bit_torrent
