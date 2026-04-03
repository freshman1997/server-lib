#include "bit_torrent_client.h"
#include "utils.h"
#include "peer_wire/peer_connection.h"
#include "net/poller/select_poller.h"
#include "net/connection/connection.h"
#include "timer/wheel_timer_manager.h"
#include "timer/timer.h"
#include "timer/timer_task.h"
#include <cstdio>
#include <cstring>

namespace yuan::net::bit_torrent
{

// TimerTask wrapper for lambda-based callbacks
class FunctionTimerTask : public timer::TimerTask
{
public:
    explicit FunctionTimerTask(std::function<void(timer::Timer *)> fn) : fn_(std::move(fn)) {}
    void on_timer(timer::Timer *timer) override { fn_(timer); }
private:
    std::function<void(timer::Timer *)> fn_;
};

BitTorrentClient::BitTorrentClient()
    : ev_loop_(nullptr),
      timer_manager_(nullptr),
      poller_(nullptr),
      announce_timer_(nullptr),
      running_(false)
{
}

BitTorrentClient::~BitTorrentClient()
{
    stop();
}

bool BitTorrentClient::load_torrent(const std::string &file_path)
{
    meta_ = TorrentMeta::parse_file(file_path);
    if (meta_.info_hash_.empty()) return false;

    pieces_have_.assign(meta_.info.piece_count(), false);
    pieces_downloading_.assign(meta_.info.piece_count(), false);
    stats_.total_bytes_ = meta_.info.total_length_;
    stats_.total_pieces_ = meta_.info.piece_count();
    return true;
}

bool BitTorrentClient::load_torrent_data(const std::string &torrent_data)
{
    meta_ = TorrentMeta::parse(torrent_data);
    if (meta_.info_hash_.empty()) return false;

    pieces_have_.assign(meta_.info.piece_count(), false);
    pieces_downloading_.assign(meta_.info.piece_count(), false);
    stats_.total_bytes_ = meta_.info.total_length_;
    stats_.total_pieces_ = meta_.info.piece_count();
    return true;
}

bool BitTorrentClient::start()
{
    if (meta_.info_hash_.empty() || running_.load()) return false;

    peer_id_ = generate_peer_id();
    running_ = true;

    // Create event loop if not provided
    if (!ev_loop_)
    {
        own_loop_ = true;
        poller_ = new net::SelectPoller();
        timer_manager_ = new timer::WheelTimerManager();
        ev_loop_ = new net::EventLoop(poller_, timer_manager_);
    }

    // Create temp file prefix
    if (save_path_.empty())
        save_path_ = "./";
    temp_file_prefix_ = save_path_ + "/" + meta_.info.name_ + ".partial.";

    // Start announce to trackers via event loop
    ev_loop_->queue_in_loop([this]()
    {
        // Start NAT traversal (listening, UPnP, uTP, DHT, PEX)
        nat_manager_ = std::make_unique<NatManager>();
        nat_config_.listen_port = listen_port_;
        nat_manager_->start(nat_config_, meta_, peer_id_, pieces_have_, ev_loop_, timer_manager_);

        // Set NAT callbacks
        nat_manager_->set_peer_callback([this](PeerConnection *peer)
        {
            on_inbound_peer(peer);
        });

        nat_manager_->set_dht_peer_callback([this](const std::vector<PeerAddress> &peers)
        {
            connect_peers(peers);
        });

        // Use the effective external port for tracker announces
        if (nat_manager_->is_upnp_mapped())
        {
            listen_port_ = nat_manager_->get_external_port();
        }
        else if (nat_manager_->is_listening())
        {
            listen_port_ = nat_manager_->get_external_port();
        }

        announce_to_trackers();
    });

    // Periodic stats update
    timer_manager_->interval(5000, 5000, new FunctionTimerTask([this](timer::Timer *)
    {
        update_stats();
    }));

    if (own_loop_)
    {
        ev_loop_->loop();
    }

    return true;
}

void BitTorrentClient::stop()
{
    if (!running_.exchange(false)) return;

    // Stop NAT manager before disconnecting peers
    if (nat_manager_)
    {
        nat_manager_->stop();
        nat_manager_.reset();
    }

    disconnect_all_peers();
    flush_all();

    if (announce_timer_)
    {
        announce_timer_->cancel();
        announce_timer_ = nullptr;
    }

    // Close piece files
    for (auto &pair : piece_files_)
    {
        if (pair.second) fclose(pair.second);
    }
    piece_files_.clear();

    if (own_loop_ && ev_loop_)
    {
        ev_loop_->quit();
        delete ev_loop_;
        ev_loop_ = nullptr;
        delete timer_manager_;
        timer_manager_ = nullptr;
        delete poller_;
        poller_ = nullptr;
        own_loop_ = false;
    }
}

void BitTorrentClient::announce_to_trackers()
{
    if (!running_.load()) return;

    for (size_t tier = 0; tier < meta_.announce_list_.size(); tier++)
    {
        for (size_t i = 0; i < meta_.announce_list_[tier].size(); i++)
        {
            const auto &url = meta_.announce_list_[tier][i];
            int64_t left = meta_.info.total_length_ - stats_.downloaded_bytes_;

            if (url.substr(0, 4) == "http")
            {
                // HTTP/HTTPS tracker
                TrackerResponse resp;
                http_tracker_.announce(url, meta_, listen_port_,
                    stats_.uploaded_bytes_, stats_.downloaded_bytes_, left, &resp);

                if (!resp.peers_.empty())
                {
                    connect_peers(resp.peers_);
                    if (announce_interval_ == 0 && resp.interval_ > 0)
                        announce_interval_ = resp.interval_;
                }

                // Only try first tracker in each tier that works
                break;
            }
            else if (url.substr(0, 3) == "udp")
            {
                // UDP tracker - parse host:port from udp://host:port
                std::string host_port = url.substr(6); // remove "udp://"
                auto colon = host_port.find(':');
                if (colon != std::string::npos)
                {
                    std::string host = host_port.substr(0, colon);
                    uint16_t port = static_cast<uint16_t>(std::atoi(host_port.substr(colon + 1).c_str()));

                    UdpTracker udp_tracker;
                    UdpTrackerResponse resp;
                    udp_tracker.announce(host, port, meta_, listen_port_,
                        stats_.uploaded_bytes_, stats_.downloaded_bytes_, left, &resp);

                    if (!resp.peers_.empty())
                    {
                        connect_peers(resp.peers_);
                        if (announce_interval_ == 0 && resp.interval_ > 0)
                            announce_interval_ = resp.interval_;
                    }
                }
                break;
            }
        }
    }

    // Schedule next announce
    if (announce_interval_ > 0 && running_.load())
    {
        timer_manager_->timeout(static_cast<uint32_t>(announce_interval_ * 1000),
            new FunctionTimerTask([this](timer::Timer *)
        {
            announce_to_trackers();
        }));
    }
}

void BitTorrentClient::connect_peers(const std::vector<PeerAddress> &peer_list)
{
    if (!running_.load()) return;

    std::lock_guard<std::mutex> lock(peers_mutex_);

    for (const auto &addr : peer_list)
    {
        if (static_cast<int32_t>(peers_.size()) >= max_peers_) break;

        std::string key = addr.ip_ + ":" + std::to_string(addr.port_);
        if (peers_.count(key)) continue;

        // Skip self
        if (addr.ip_ == "0.0.0.0" || addr.ip_ == "127.0.0.1") continue;

        auto *peer = new PeerConnection();
        peer->set_piece_data_handler([this](uint32_t piece_index, uint32_t offset,
                                            const uint8_t *data, uint32_t length)
        {
            on_piece_data(piece_index, offset, data, length);
        });

        peer->set_on_state_change([this](PeerConnection *p)
        {
            on_peer_connected(p);
        });

        // Connect peer - will send our bitfield after handshake
        peer->connect(addr.ip_, addr.port_, meta_, peer_id_, timer_manager_, ev_loop_);
        peers_[key] = peer;

        // Register with NAT manager (for PEX)
        if (nat_manager_)
            nat_manager_->register_peer(peer, key);
    }

    stats_.total_peers_ = static_cast<int32_t>(peers_.size());
}

void BitTorrentClient::on_peer_connected(PeerConnection *peer)
{
    if (peer->get_state() == PeerConnection::State::connected)
    {
        // Send our bitfield
        PeerState our_state;
        our_state.pieces = pieces_have_;
        auto bf = our_state.to_bitfield();
        if (!bf.empty())
            peer->send_bitfield(bf);

        // Send interested
        peer->send_interested();

        // Try to request a piece
        peer->request_next_piece(pieces_have_);
    }
}

void BitTorrentClient::on_inbound_peer(PeerConnection *peer)
{
    if (!running_.load()) return;

    std::lock_guard<std::mutex> lock(peers_mutex_);
    if (static_cast<int32_t>(peers_.size()) >= max_peers_) return;

    std::string key = peer->get_peer_ip() + ":" + std::to_string(peer->get_peer_port());
    if (peers_.count(key)) return;

    peer->set_piece_data_handler([this](uint32_t piece_index, uint32_t offset,
                                        const uint8_t *data, uint32_t length)
    {
        on_piece_data(piece_index, offset, data, length);
    });

    // Inbound peer is already connected, handle it immediately
    on_peer_connected(peer);
    peers_[key] = peer;

    if (nat_manager_)
        nat_manager_->register_peer(peer, key);
}

void BitTorrentClient::on_piece_data(uint32_t piece_index, uint32_t offset,
                                      const uint8_t *data, uint32_t length)
{
    write_piece(static_cast<int32_t>(piece_index), offset, data, length);
}

void BitTorrentClient::disconnect_all_peers()
{
    std::lock_guard<std::mutex> lock(peers_mutex_);
    for (auto &pair : peers_)
    {
        pair.second->disconnect();
        delete pair.second;
    }
    peers_.clear();
}

bool BitTorrentClient::write_piece(int32_t piece_index, uint32_t offset,
                                   const uint8_t *data, uint32_t length)
{
    if (piece_index < 0 || piece_index >= meta_.info.piece_count()) return false;

    // Open piece file
    if (piece_files_.find(piece_index) == piece_files_.end())
    {
        std::string path = temp_file_prefix_ + std::to_string(piece_index);
        piece_files_[piece_index] = fopen(path.c_str(), "wb");
        if (!piece_files_[piece_index]) return false;
    }

    auto *f = piece_files_[piece_index];
    fseek(f, offset, SEEK_SET);
    size_t written = fwrite(data, 1, length, f);
    fflush(f);

    if (written == length)
    {
        stats_.downloaded_bytes_ += length;
        pieces_downloading_[piece_index] = true;
    }

    return written == length;
}

bool BitTorrentClient::verify_piece(int32_t piece_index)
{
    if (piece_index < 0 || piece_index >= meta_.info.piece_count()) return false;

    std::string expected_hash = meta_.info.piece_hash(piece_index);
    if (expected_hash.empty()) return false;

    // Read piece file and compute SHA-1
    std::string path = temp_file_prefix_ + std::to_string(piece_index);
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> piece_data(size);
    fread(piece_data.data(), 1, size, f);
    fclose(f);

    auto hash = sha1_hash(piece_data.data(), piece_data.size());
    std::string hash_hex = to_hex(hash);

    if (hash_hex == expected_hash)
    {
        pieces_have_[piece_index] = true;
        pieces_downloading_[piece_index] = false;
        stats_.pieces_downloaded_++;

        // Send 'have' to all connected peers
        for (auto &pair : peers_)
        {
            if (pair.second->is_connected())
                pair.second->send_have(piece_index);
        }

        // Notify NAT manager of piece change (for DHT re-announce)
        if (nat_manager_)
            nat_manager_->on_pieces_changed(pieces_have_);

        return true;
    }

    return false;
}

void BitTorrentClient::flush_all()
{
    for (auto &pair : piece_files_)
    {
        if (pair.second)
        {
            fflush(pair.second);
            fclose(pair.second);
            pair.second = nullptr;
        }
    }
    piece_files_.clear();
}

void BitTorrentClient::update_stats()
{
    stats_.active_peers_ = 0;
    std::lock_guard<std::mutex> lock(peers_mutex_);
    for (const auto &pair : peers_)
    {
        if (pair.second->is_connected())
            stats_.active_peers_++;
    }

    if (stats_.total_pieces_ > 0)
        stats_.progress_ = static_cast<float>(stats_.pieces_downloaded_) / stats_.total_pieces_;

    if (stats_callback_)
        stats_callback_(stats_);
}

} // namespace yuan::net::bit_torrent
