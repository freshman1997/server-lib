#ifndef __BIT_TORRENT_BIT_TORRENT_CLIENT_H__
#define __BIT_TORRENT_BIT_TORRENT_CLIENT_H__

#include "torrent_meta.h"
#include "nat/nat_config.h"
#include "stats/download_stats_tracker.h"
#include "state/piece_download_state.h"
#include "net/poller/poller.h"
#include "event/event_loop.h"
#include "timer/timer_manager.h"
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

// Main BitTorrent client - integrates tracker discovery + peer wire protocol
// Reference design: aria2's BT download engine
//
// Lifecycle:
//   1. load_torrent() -> parse .torrent file
//   2. set_save_path() -> where to save downloaded files
//   3. start() -> discover peers via trackers, connect, download
//   4. stop() -> graceful shutdown
//
// Thread model:
//   - Main thread: API calls, callbacks
//   - I/O thread: EventLoop handles all network I/O (single-threaded event loop)
class BitTorrentClient
{
public:
    BitTorrentClient();
    ~BitTorrentClient();

public:
    // Load a .torrent file
    bool load_torrent(const std::string &file_path);
    bool load_torrent_data(const std::string &torrent_data);

    // Set download save path
    void set_save_path(const std::string &path) { save_path_ = path; }

    // Set download options
    void set_max_peers(int32_t max) { max_peers_ = max; }
    void set_listen_port(int32_t port) { listen_port_ = port; }

    // Set NAT configuration (call before start())
    void set_nat_config(const NatConfig &config) { nat_config_ = config; }

    // Get NAT manager (for advanced usage)
    NatManager *get_nat_manager();

    // Inject a shared runtime. If not set, start() creates and owns one.
    void set_runtime(net::EventLoop *loop, timer::TimerManager *tm)
    {
        ev_loop_ = loop;
        timer_manager_ = tm;
        own_loop_ = false;
    }

    // Set the EventLoop (if not set, creates own loop in start())
    void set_event_loop(net::EventLoop *loop) { ev_loop_ = loop; }
    void set_timer_manager(timer::TimerManager *tm) { timer_manager_ = tm; }

    // Stats callback (called periodically)
    void set_stats_callback(StatsCallback cb) { stats_tracker_.set_callback(std::move(cb)); }

    // Start download (blocking if no external event loop set)
    bool start();
    void stop();

    // Query current state
    bool is_running() const { return running_.load(); }
    bool is_complete() const { return piece_state_.is_complete(); }
    const DownloadStats &get_stats() const { return stats_tracker_.stats(); }
    const TorrentMeta &get_meta() const { return meta_; }
    int32_t get_peer_count() const;

private:
    bool init_runtime();
    bool should_block_on_start() const;
    void preload_existing_pieces();
    void start_download_runtime();
    void stop_download_runtime();
    void cleanup_runtime();

    // Peer events
    void on_peer_connected(PeerConnection *peer);
    void on_piece_data(PeerConnection *peer, uint32_t piece_index, uint32_t offset,
                       const uint8_t *data, uint32_t length);
    bool on_piece_request(uint32_t piece_index, uint32_t offset, uint32_t length, std::vector<uint8_t> &out);
    void on_piece_served(uint32_t piece_index, uint32_t offset, uint32_t length);
    void on_peer_requests_lost(const std::vector<PieceBlockRequest> &requests);
    std::vector<uint32_t> build_piece_availability() const;
    void request_next_block(PeerConnection *peer);

private:
    TorrentMeta meta_;
    std::string save_path_;
    int32_t max_peers_ = 50;
    int32_t listen_port_ = 6881;
    std::string peer_id_;

    // Download state
    PieceDownloadState piece_state_;
    DownloadStatsTracker stats_tracker_;

    // Event loop (owned or borrowed)
    net::EventLoop *ev_loop_;
    timer::TimerManager *timer_manager_;
    net::Poller *poller_;
    bool own_loop_ = false;

    std::unique_ptr<PieceStorage> piece_storage_;

    // State
    std::atomic<bool> running_;

    // NAT/runtime orchestration
    NatConfig nat_config_;
    std::unique_ptr<DownloadRuntimeCoordinator> runtime_coordinator_;
};

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_BIT_TORRENT_CLIENT_H__
