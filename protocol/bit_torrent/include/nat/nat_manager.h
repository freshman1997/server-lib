#ifndef __BIT_TORRENT_NAT_MANAGER_H__
#define __BIT_TORRENT_NAT_MANAGER_H__

// NAT Manager - unified entry point for all NAT traversal features.
//
// Orchestrates PeerListener, UpnpManager, UtpManager, DhtNode, and PexManager.
// All features are independently configurable via NatConfig.
//
// Default configuration (most mature features enabled):
//   - Inbound listening: ON
//   - UPnP / NAT-PMP: ON
//   - uTP: ON
//   - DHT: ON
//   - PEX: ON

#include "nat_config.h"
#include "peer_listener.h"
#include "upnp_manager.h"
#include "utp_connection.h"
#include "dht_node.h"
#include "pex_manager.h"
#include "torrent_meta.h"
#include "peer_wire/peer_connection.h"
#include "event/event_loop.h"
#include "timer/timer_manager.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace yuan::net::bit_torrent
{

class NatManager
{
public:
    using PeerCallback = std::function<void(PeerConnection *peer)>;
    using DhtPeerCallback = std::function<void(const std::vector<PeerAddress> &peers)>;

    NatManager();
    ~NatManager();

    // Initialize and start all enabled NAT traversal features.
    // Must be called from the event loop thread.
    void start(const NatConfig &config,
               const TorrentMeta &meta,
               const std::string &peer_id,
               const std::vector<bool> &pieces_have,
               net::EventLoop *loop,
               timer::TimerManager *timer_mgr);

    void stop();

    // Called when a new peer is fully connected (from any transport)
    void register_peer(PeerConnection *peer, const std::string &key);

    // Called when a peer disconnects
    void unregister_peer(const std::string &key);

    // Called when our piece possession changes (for PEX/DHT announce)
    void on_pieces_changed(const std::vector<bool> &pieces_have);

    // Get effective external address (from UPnP or manual config)
    std::string get_external_ip() const;
    uint16_t get_external_port() const;

    // Get PEX manager for integration with peer connections
    PexManager *get_pex_manager() { return pex_manager_.get(); }

    // Getters for individual subsystems
    bool is_listening() const;
    bool is_upnp_mapped() const;
    bool is_dht_running() const;
    bool is_utp_running() const;

    const NatConfig &get_config() const { return config_; }

    // Callbacks
    void set_peer_callback(PeerCallback cb) { peer_cb_ = std::move(cb); }
    void set_dht_peer_callback(DhtPeerCallback cb) { dht_peer_cb_ = std::move(cb); }

private:
    void on_upnp_result(bool success, const std::string &ip, uint16_t port);
    void on_new_tcp_peer(PeerConnection *peer);
    void on_new_utp_peer(UtpConnection *utp_conn);
    void on_dht_peers(const std::vector<PeerAddress> &peers);

private:
    NatConfig config_;

    net::EventLoop *ev_loop_ = nullptr;
    timer::TimerManager *timer_manager_ = nullptr;

    std::vector<uint8_t> info_hash_;
    std::string peer_id_;
    const std::vector<bool> *pieces_have_ = nullptr;

    std::unique_ptr<PeerListener> peer_listener_;
    std::unique_ptr<UpnpManager> upnp_manager_;
    std::unique_ptr<UtpManager> utp_manager_;
    std::unique_ptr<DhtNode> dht_node_;
    std::unique_ptr<PexManager> pex_manager_;

    // External address (from UPnP or manual)
    std::string external_ip_;
    uint16_t external_port_ = 0;
    bool external_address_resolved_ = false;

    PeerCallback peer_cb_;
    DhtPeerCallback dht_peer_cb_;

    bool started_ = false;
};

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_NAT_MANAGER_H__
