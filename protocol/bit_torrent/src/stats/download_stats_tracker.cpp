#include "stats/download_stats_tracker.h"

#include <algorithm>
#include <climits>
#include <chrono>

namespace yuan::net::bit_torrent
{

void DownloadStatsTracker::reset(int64_t total_bytes, int32_t total_pieces)
{
    stats_ = {};
    stats_.total_bytes_ = total_bytes;
    stats_.total_pieces_ = total_pieces;
    completed_pieces_.assign(static_cast<size_t>(std::max(total_pieces, 0)), false);
    last_downloaded_bytes_ = 0;
    last_uploaded_bytes_ = 0;
    last_rate_update_ms_ = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
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
        if (meta.info.total_length_ > 0) {
            ctx.left_ = std::max<int64_t>(0, meta.info.total_length_ - stats_.downloaded_bytes_);
        } else {
            ctx.left_ = INT64_MAX;
        }
        return ctx;
    }

void DownloadStatsTracker::emit()
{
    update_rates();
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

void DownloadStatsTracker::update_rates()
{
    const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (last_rate_update_ms_ == 0) {
        last_rate_update_ms_ = now;
        last_downloaded_bytes_ = stats_.downloaded_bytes_;
        last_uploaded_bytes_ = stats_.uploaded_bytes_;
        return;
    }

    const auto elapsed_ms = now > last_rate_update_ms_ ? now - last_rate_update_ms_ : 0;
    if (elapsed_ms < 250) {
        return;
    }

    const double elapsed_sec = static_cast<double>(elapsed_ms) / 1000.0;
    stats_.download_speed_ = static_cast<float>(
        static_cast<double>(std::max<int64_t>(0, stats_.downloaded_bytes_ - last_downloaded_bytes_)) / elapsed_sec);
    stats_.upload_speed_ = static_cast<float>(
        static_cast<double>(std::max<int64_t>(0, stats_.uploaded_bytes_ - last_uploaded_bytes_)) / elapsed_sec);
    last_downloaded_bytes_ = stats_.downloaded_bytes_;
    last_uploaded_bytes_ = stats_.uploaded_bytes_;
    last_rate_update_ms_ = now;
}

} // namespace yuan::net::bit_torrent
