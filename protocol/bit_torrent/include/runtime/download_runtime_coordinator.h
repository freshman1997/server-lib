#ifndef __BIT_TORRENT_RUNTIME_DOWNLOAD_RUNTIME_COORDINATOR_H__
#define __BIT_TORRENT_RUNTIME_DOWNLOAD_RUNTIME_COORDINATOR_H__

#include "nat/nat_config.h"
#include "session/peer_session.h"
#include "session/tracker_session.h"
#include "stats/download_stats_tracker.h"
#include "net/runtime/network_runtime.h"

#include <memory>
#include <string>
#include <vector>

namespace yuan::net::bit_torrent
{

    class NatManager;

    struct DownloadRuntimeConfig
    {
        net::NetworkRuntime *runtime_ = nullptr;
        const TorrentMeta *meta_ = nullptr;
        const std::string *peer_id_ = nullptr;
        const std::vector<bool> *pieces_have_ = nullptr;
        int32_t *listen_port_ = nullptr;
        int32_t max_peers_ = 50;
        NatConfig nat_config_;
        DownloadStatsTracker *stats_tracker_ = nullptr;
        PieceDataHandler piece_data_handler_;
        PieceRequestHandler piece_request_handler_;
        PieceServedHandler piece_served_handler_;
        std::function<void(PeerConnection *)> peer_ready_handler_;
        std::function<void(const std::vector<PieceBlockRequest> &)> peer_lost_handler_;
    };

    class DownloadRuntimeCoordinator
    {
    public:
        void configure(DownloadRuntimeConfig config);

        bool start(bool defer_bootstrap_to_loop);
        void stop();
        void announce_now();

        NatManager *get_nat_manager()
        {
            return nat_manager_ ? &*nat_manager_ : nullptr;
        }
        void broadcast_have(uint32_t piece_index);
        std::vector<std::shared_ptr<PeerConnection> > get_active_peers() const;
        int32_t get_peer_count() const;
        int32_t get_active_peer_count() const;

    private:
        void ensure_sessions();
        void configure_peer_session();
        void configure_tracker_session();
        void start_nat_runtime();
        void update_listen_port_from_nat();
        void bootstrap_runtime();
        void schedule_stats_updates();

    private:
        DownloadRuntimeConfig config_;
        timer::Timer *stats_timer_ = nullptr;
        std::unique_ptr<NatManager> nat_manager_;
        std::unique_ptr<TrackerSession> tracker_session_;
        std::unique_ptr<PeerSession> peer_session_;
    };

} // namespace yuan::net::bit_torrent

#endif
