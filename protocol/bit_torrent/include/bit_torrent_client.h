#ifndef __BIT_TORRENT_BIT_TORRENT_CLIENT_H__
#define __BIT_TORRENT_BIT_TORRENT_CLIENT_H__

#include "torrent_meta.h"
#include "tracker/http_tracker.h"
#include "tracker/udp_tracker.h"
#include "peer_wire/peer_connection.h"
#include "nat/nat_manager.h"
#include "net/poller/poller.h"
#include "event/event_loop.h"
#include "timer/timer_manager.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstdint>

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
    float download_speed_ = 0.0f;  // bytes per second
    float progress_ = 0.0f;         // 0.0 ~ 1.0
};

using StatsCallback = std::function<void(const DownloadStats &)>;

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
    NatManager *get_nat_manager() { return nat_manager_.get(); }

    // Set the EventLoop (if not set, creates own loop in start())
    void set_event_loop(net::EventLoop *loop) { ev_loop_ = loop; }
    void set_timer_manager(timer::TimerManager *tm) { timer_manager_ = tm; }

    // Stats callback (called periodically)
    void set_stats_callback(StatsCallback cb) { stats_callback_ = std::move(cb); }

    // Start download (blocking if no external event loop set)
    bool start();
    void stop();

    // Query current state
    bool is_running() const { return running_.load(); }
    const DownloadStats &get_stats() const { return stats_; }
    const TorrentMeta &get_meta() const { return meta_; }
    int32_t get_peer_count() const { return static_cast<int32_t>(peers_.size()); }

private:
    // Tracker phase
    void announce_to_trackers();
    void on_tracker_http_response(const TrackerResponse &resp);
    void on_tracker_udp_response(const UdpTrackerResponse &resp);

    // Peer management
    void connect_peers(const std::vector<PeerAddress> &peer_list);
    void on_peer_connected(PeerConnection *peer);
    void on_inbound_peer(PeerConnection *peer);
    void on_peer_disconnected(const std::string &key);
    void on_piece_data(uint32_t piece_index, uint32_t offset, const uint8_t *data, uint32_t length);
    void disconnect_all_peers();

    // File I/O
    bool write_piece(int32_t piece_index, uint32_t offset, const uint8_t *data, uint32_t length);
    bool verify_piece(int32_t piece_index);
    void flush_all();

    // Stats
    void update_stats();

private:
    TorrentMeta meta_;
    std::string save_path_;
    int32_t max_peers_ = 50;
    int32_t listen_port_ = 6881;
    std::string peer_id_;

    // Download state
    std::vector<bool> pieces_have_;
    std::vector<bool> pieces_downloading_;
    DownloadStats stats_;

    // Event loop (owned or borrowed)
    net::EventLoop *ev_loop_;
    timer::TimerManager *timer_manager_;
    net::Poller *poller_;
    bool own_loop_ = false;

    // Trackers
    HttpTracker http_tracker_;
    int32_t announce_interval_ = 0;
    timer::Timer *announce_timer_ = nullptr;

    // Peer connections
    std::unordered_map<std::string, PeerConnection *> peers_;
    std::mutex peers_mutex_;

    // File handles (simplified: one file per piece for now)
    std::unordered_map<int32_t, FILE *> piece_files_;
    std::string temp_file_prefix_;

    // Callbacks
    StatsCallback stats_callback_;

    // State
    std::atomic<bool> running_;

    // NAT traversal
    NatConfig nat_config_;
    std::unique_ptr<NatManager> nat_manager_;
};

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_BIT_TORRENT_CLIENT_H__
