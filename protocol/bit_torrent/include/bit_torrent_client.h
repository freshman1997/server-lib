#ifndef __BIT_TORRENT_BIT_TORRENT_CLIENT_H__
#define __BIT_TORRENT_BIT_TORRENT_CLIENT_H__

#include "torrent_meta.h"
#include "nat/nat_config.h"
#include "stats/download_stats_tracker.h"
#include "state/piece_download_state.h"
#include "net/runtime/network_runtime.h"
#include "timer/timer.h"

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <cstdint>

namespace yuan::net::bit_torrent
{

    class NatManager;
    class PeerConnection;
    class PieceStorage;
    class DownloadRuntimeCoordinator;

    class BitTorrentClient
    {
    public:
        BitTorrentClient();
        ~BitTorrentClient();

    public:
        bool load_torrent(const std::string &file_path);
        bool load_torrent_data(const std::string &torrent_data);

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

        bool start();
        void stop();

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
        int32_t get_peer_count() const;

    private:
        void preload_existing_pieces();
        void start_download_runtime();
        void stop_download_runtime();

        void on_peer_connected(PeerConnection *peer);
        void on_piece_data(PeerConnection *peer, uint32_t piece_index, uint32_t offset,
                           const uint8_t *data, uint32_t length);
        bool on_piece_request(uint32_t piece_index, uint32_t offset, uint32_t length, std::vector<uint8_t> &out);
        void on_piece_served(uint32_t piece_index, uint32_t offset, uint32_t length);
        void on_peer_requests_lost(const std::vector<PieceBlockRequest> &requests);
        std::vector<uint32_t> build_piece_availability() const;
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
        timer::Timer *unchoke_timer_ = nullptr;
    };

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_BIT_TORRENT_CLIENT_H__
