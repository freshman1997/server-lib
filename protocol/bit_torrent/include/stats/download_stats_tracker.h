#ifndef __BIT_TORRENT_STATS_DOWNLOAD_STATS_TRACKER_H__
#define __BIT_TORRENT_STATS_DOWNLOAD_STATS_TRACKER_H__

#include "session/tracker_session.h"

#include <cstdint>
#include <functional>

namespace yuan::net::bit_torrent
{

struct DownloadStats
{
    int64_t total_bytes_ = 0;
    int64_t downloaded_bytes_ = 0;
    int64_t uploaded_bytes_ = 0;
    int32_t active_peers_ = 0;
    int32_t total_peers_ = 0;
    int32_t pieces_downloaded_ = 0;
    int32_t total_pieces_ = 0;
    float download_speed_ = 0.0f;
    float progress_ = 0.0f;
};

using StatsCallback = std::function<void(const DownloadStats &)>;

class DownloadStatsTracker
{
public:
    void reset(int64_t total_bytes, int32_t total_pieces);
    void set_callback(StatsCallback cb) { callback_ = std::move(cb); }

    void add_downloaded_bytes(uint32_t bytes);
    void add_uploaded_bytes(uint32_t bytes);
    void set_piece_completed(uint32_t piece_index);
    void update_peer_counts(int32_t active_peers, int32_t total_peers);

    TrackerAnnounceContext make_tracker_context(const TorrentMeta &meta, int32_t listen_port) const;
    void emit();

    const DownloadStats &stats() const { return stats_; }

private:
    void update_progress();

private:
    DownloadStats stats_;
    std::vector<bool> completed_pieces_;
    StatsCallback callback_;
};

} // namespace yuan::net::bit_torrent

#endif
