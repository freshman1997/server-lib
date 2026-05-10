#ifndef __BIT_TORRENT_BIT_TORRENT_CLIENT_H__
#define __BIT_TORRENT_BIT_TORRENT_CLIENT_H__

#include "torrent_meta.h"
#include "magnet_uri.h"
#include "nat/nat_config.h"
#include "stats/download_stats_tracker.h"
#include "state/piece_download_state.h"
#include "net/runtime/network_runtime.h"
#include "timer/timer_handle.h"

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <cstdint>
#include <algorithm>

namespace yuan::net::bit_torrent
{

    class NatManager;
    class PeerConnection;
    class PieceStorage;
    class DownloadRuntimeCoordinator;

    class BitTorrentClient
    {
    public:
        using PeerConnectedCallback = std::function<void(const std::string &, uint16_t, const std::string &)>;
        using PieceCompletedCallback = std::function<void(uint32_t, uint32_t)>;
        using TorrentCompletedCallback = std::function<void()>;
        using MetadataReceivedCallback = std::function<void()>;

    public:
        BitTorrentClient();
        ~BitTorrentClient();

    public:
        bool load_torrent(const std::string &file_path);
        bool load_torrent_data(const std::string &torrent_data);
        bool load_magnet(const std::string &magnet_uri);

        void set_save_path(const std::string &path)
        {
            save_path_ = path;
        }

        void set_max_peers(int32_t max)
        {
            max_peers_ = max;
        }
        void set_listen_port(int32_t port)
        {
            listen_port_ = port;
        }

        void set_nat_config(const NatConfig &config)
        {
            nat_config_ = config;
        }

        const NatConfig &get_nat_config() const
        {
            return nat_config_;
        }

        NatManager *get_nat_manager();

        void set_runtime(NetworkRuntime &runtime)
        {
            runtime_ = &runtime;
            owned_runtime_.reset();
        }

        void set_stats_callback(StatsCallback cb)
        {
            stats_tracker_.set_callback(std::move(cb));
        }

        void set_peer_connected_callback(PeerConnectedCallback cb)
        {
            peer_connected_callback_ = std::move(cb);
        }
        void set_piece_completed_callback(PieceCompletedCallback cb)
        {
            piece_completed_callback_ = std::move(cb);
        }
        void set_torrent_completed_callback(TorrentCompletedCallback cb)
        {
            torrent_completed_callback_ = std::move(cb);
        }
        void set_metadata_received_callback(MetadataReceivedCallback cb)
        {
            metadata_received_callback_ = std::move(cb);
        }

        bool start();
        void stop();
        void clear_torrent();

        bool is_running() const
        {
            return running_.load();
        }
        bool is_complete() const
        {
            return piece_state_.is_complete();
        }
        const DownloadStats &get_stats() const
        {
            return stats_tracker_.stats();
        }
        const TorrentMeta &get_meta() const
        {
            return meta_;
        }
        bool has_loaded_torrent() const
        {
            return !meta_.info_hash_.empty();
        }
        bool is_metadata_mode() const
        {
            return metadata_mode_;
        }
        const std::string &get_save_path() const
        {
            return save_path_;
        }
        int32_t get_max_peers() const
        {
            return max_peers_;
        }
        int32_t get_listen_port() const
        {
            return listen_port_;
        }
        void set_download_limit_kbps(int32_t kbps)
        {
            download_limit_kbps_ = std::max(0, kbps);
        }
        void set_upload_limit_kbps(int32_t kbps)
        {
            upload_limit_kbps_ = std::max(0, kbps);
        }
        int32_t get_download_limit_kbps() const
        {
            return download_limit_kbps_;
        }
        int32_t get_upload_limit_kbps() const
        {
            return upload_limit_kbps_;
        }
        int32_t get_peer_count() const;

        std::vector<std::shared_ptr<PeerConnection>> get_active_peers() const;

        const std::vector<std::string> &get_magnet_tracker_urls() const
        {
            return magnet_tracker_urls_;
        }

        const std::vector<bool> &get_pieces_have() const
        {
            return piece_state_.pieces_have();
        }

        const std::vector<bool> &get_pieces_downloading() const
        {
            return piece_state_.pieces_downloading();
        }

        int32_t remaining_piece_count() const
        {
            return piece_state_.remaining_piece_count();
        }

    private:
        void preload_existing_pieces();
        bool start_download_runtime();
        void stop_download_runtime();
        bool emit_torrent_completed_once();
        void refill_bandwidth_budget();
        bool consume_download_budget(uint32_t bytes);
        bool consume_upload_budget(uint32_t bytes);
        void on_metadata_received(const std::vector<uint8_t> &metadata);

        void on_peer_connected(PeerConnection *peer);
        void on_piece_data(PeerConnection *peer, uint32_t piece_index, uint32_t offset,
                           const uint8_t *data, uint32_t length);
        bool on_piece_request(uint32_t piece_index, uint32_t offset, uint32_t length, std::vector<uint8_t> &out);
        void on_piece_served(uint32_t piece_index, uint32_t offset, uint32_t length);
        void on_peer_requests_lost(const std::vector<PieceBlockRequest> &requests);
        std::vector<uint32_t> build_piece_availability(const std::vector<std::shared_ptr<PeerConnection> > &peers) const;
        void request_next_block(PeerConnection *peer);
        void perform_choking_round();
        void on_unchoke_timer();

    private:
        TorrentMeta meta_;
        std::string save_path_;
        int32_t max_peers_ = 50;
        int32_t listen_port_ = 6881;
        std::string peer_id_;

        PieceDownloadState piece_state_;
        DownloadStatsTracker stats_tracker_;

        NetworkRuntime *runtime_ = nullptr;
        std::unique_ptr<NetworkRuntime> owned_runtime_;

        std::unique_ptr<PieceStorage> piece_storage_;

        std::atomic<bool> running_;

        NatConfig nat_config_;
        std::unique_ptr<DownloadRuntimeCoordinator> runtime_coordinator_;

        int32_t upload_slots_ = 4;
        timer::TimerHandle unchoke_timer_;
        bool torrent_completed_emitted_ = false;
        int32_t download_limit_kbps_ = 0;
        int32_t upload_limit_kbps_ = 0;
        double download_budget_bytes_ = 0.0;
        double upload_budget_bytes_ = 0.0;
        uint64_t bandwidth_last_refill_ms_ = 0;

        int32_t optimistic_unchoke_index_ = -1;
        uint32_t optimistic_unchoke_counter_ = 0;
        static constexpr uint32_t OPTIMISTIC_UNCHOKE_INTERVAL = 3;

        PeerConnectedCallback peer_connected_callback_;
        PieceCompletedCallback piece_completed_callback_;
        TorrentCompletedCallback torrent_completed_callback_;
        MetadataReceivedCallback metadata_received_callback_;

        bool metadata_mode_ = false;
        std::vector<std::string> magnet_tracker_urls_;
    };

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_BIT_TORRENT_CLIENT_H__
