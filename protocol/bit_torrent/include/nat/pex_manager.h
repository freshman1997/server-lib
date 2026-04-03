#ifndef __BIT_TORRENT_NAT_PEX_MANAGER_H__
#define __BIT_TORRENT_NAT_PEX_MANAGER_H__

// PEX (Peer Exchange) - BEP 10
//
// Extends the BitTorrent protocol with an extension handshake and ut_pex messages.
// Allows peers to exchange their known peer lists, reducing reliance on trackers.
//
// Extension protocol:
//   1. After BT handshake, both sides check reserved[5] bit 4 for extension support.
//   2. If supported, each side sends an extended handshake (ext msg id 0):
//      { "m": { "ut_pex": <msg_id> }, "v": "<client_version>", ... }
//   3. ut_pex messages (with the msg_id from ext handshake) exchange peer lists.
//      Format: <bencoded dict with "added" and "dropped" compact peer lists>
//
// Compact peer list: 6 bytes per peer (4-byte IP + 2-byte port, network order)

#include "torrent_meta.h"
#include "nat_config.h"
#include "peer_wire/peer_wire_message.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <cstdint>
#include <functional>

namespace yuan::net
{
    class Connection;
}

namespace yuan::net::bit_torrent
{

struct PexPeerInfo
{
    std::string ip;
    uint16_t port;
};

// Manages PEX extension for a single torrent.
// Tracks known peers and builds ut_pex messages for each connected peer.
class PexManager
{
public:
    PexManager();
    ~PexManager();

    // Initialize with torrent info hash
    void init(const std::vector<uint8_t> &info_hash, const NatConfig &config);

    // Called when a peer's handshake is received and extension protocol is supported
    void on_peer_handshake(const uint8_t *reserved_bytes);

    // Process an extended message from a peer
    // Returns true if the message was handled (PEX-related)
    bool on_extended_message(const std::string &peer_key,
                             uint8_t ext_id,
                             const uint8_t *payload, size_t len);

    // Get the extension handshake payload to send to a peer
    std::vector<uint8_t> build_ext_handshake() const;

    // Get the ut_pex message payload for a peer (call periodically)
    std::vector<uint8_t> build_pex_message(const std::string &peer_key);

    // Add a peer discovered from any source (tracker, DHT, etc.)
    void add_peer(const std::string &ip, uint16_t port);

    // Remove a peer that we've disconnected from
    void remove_peer(const std::string &peer_key);

    // Get all known PEX peers
    std::vector<PexPeerInfo> get_all_peers() const;

    // Check if a peer supports PEX
    bool peer_supports_pex(const std::string &peer_key) const;

    // Get the ut_pex extension message ID for a peer
    int get_peer_pex_ext_id(const std::string &peer_key) const;

    // Set callback when new peers are discovered via PEX
    using NewPeerCallback = std::function<void(const std::vector<PexPeerInfo> &peers)>;
    void set_new_peer_callback(NewPeerCallback cb) { new_peer_cb_ = std::move(cb); }

    size_t peer_count() const { return known_peers_.size(); }

private:
    bool parse_ext_handshake(const std::string &peer_key,
                             const uint8_t *data, size_t len);
    bool parse_pex_message(const std::string &peer_key,
                           const uint8_t *data, size_t len);

    static std::vector<PexPeerInfo> parse_compact_peers(const uint8_t *data, size_t len);
    static std::vector<uint8_t> build_compact_peers(const std::vector<PexPeerInfo> &peers);

private:
    std::vector<uint8_t> info_hash_;
    NatConfig config_;

    // All known peers (key: "ip:port")
    std::unordered_map<std::string, PexPeerInfo> known_peers_;

    // Per-peer extension state
    struct PeerExtensionState
    {
        bool supports_extensions = false;
        bool supports_pex = false;
        int ut_pex_msg_id = 0;          // message ID for ut_pex
        std::unordered_set<std::string> sent_peers;  // peers we've already sent to this peer
        std::unordered_set<std::string> dropped_peers; // peers dropped since last PEX msg
    };
    std::unordered_map<std::string, PeerExtensionState> peer_states_;

    // Peers discovered since last flush (for PEX messages)
    std::vector<PexPeerInfo> added_since_last_flush_;

    // Our assigned extension message IDs (outgoing)
    int next_ext_id_ = 1;
    std::string our_ut_pex_id_str_;  // the ext msg id we assigned for ut_pex

    NewPeerCallback new_peer_cb_;
};

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_NAT_PEX_MANAGER_H__
