#ifndef __BIT_TORRENT_NAT_DHT_NODE_H__
#define __BIT_TORRENT_NAT_DHT_NODE_H__

// DHT (Distributed Hash Table) - BEP 5
//
// Implements Kademlia-based DHT for peer discovery without relying on trackers.
// Nodes are organized in a binary tree based on XOR distance from the local node ID.
//
// RPC messages:
//   ping:          { "t": "<transaction_id>", "y": "q", "q": "ping", "a": { "id": "<node_id>" } }
//   find_node:     { ..., "q": "find_node", "a": { "id": "<node_id>", "target": "<target_id>" } }
//   get_peers:     { ..., "q": "get_peers", "a": { "id": "<node_id>", "info_hash": "<info_hash>" } }
//   announce_peer: { ..., "q": "announce_peer", "a": { "id": "<node_id>", "info_hash": "<info_hash>", "port": <port>, "token": "<token>" } }
//
// Responses:
//   { "t": "<transaction_id>", "y": "r", "r": { "id": "<node_id>", ... } }
//   Errors: { "t": "<transaction_id>", "y": "e", "e": [<code>, "<msg>] }

#include "torrent_meta.h"
#include "nat_config.h"
#include "net/acceptor/udp_acceptor.h"
#include "event/event_loop.h"
#include "timer/timer_manager.h"
#include <string>
#include <vector>
#include <array>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <cstdint>

namespace yuan::net
{
    class UdpAcceptor;
    class Connection;
}

namespace yuan::net::bit_torrent
{

// DHT node ID: 160-bit (20-byte) identifier
using DhtNodeId = std::array<uint8_t, 20>;

// Compact node info: 26 bytes (20-byte node_id + 4-byte ip + 2-byte port)
struct DhtCompactNode
{
    DhtNodeId id;
    uint32_t ip;    // network byte order
    uint16_t port;  // network byte order

    std::string ip_string() const;
    static DhtCompactNode from_compact(const uint8_t *data);
    std::vector<uint8_t> to_compact() const;
};

// Routing table bucket (Kademlia K-bucket)
struct DhtBucket
{
    static constexpr size_t K = 8;  // Max nodes per bucket

    DhtNodeId prefix;          // Lower bound for this bucket
    std::vector<DhtCompactNode> nodes;
    std::vector<DhtCompactNode> replacement_cache;

    bool full() const { return nodes.size() >= K; }
    bool contains(const DhtNodeId &id) const;
    void add(const DhtCompactNode &node);
    void remove(const DhtNodeId &id);
    void touch(const DhtNodeId &id);
};

// Main DHT node implementation
class DhtNode : public net::ConnectionHandler
{
public:
    using PeerCallback = std::function<void(const std::vector<PeerAddress> &peers)>;
    using NodeCallback = std::function<void(const DhtCompactNode &node)>;

    DhtNode();
    ~DhtNode();

    // Start DHT node on the given UDP port
    bool start(const NatConfig &config,
               net::EventLoop *loop,
               timer::TimerManager *timer_mgr,
               const std::string &external_ip = "");

    void stop();
    bool is_running() const { return running_; }
    int32_t get_port() const { return port_; }
    const DhtNodeId &get_node_id() const { return node_id_; }

    // Announce ourselves to the DHT network for a torrent
    void announce(const std::vector<uint8_t> &info_hash, uint16_t port);

    // Look up peers for a torrent
    void get_peers(const std::vector<uint8_t> &info_hash, PeerCallback cb);

    // Add a node to the routing table (bootstrap)
    void add_node(const std::string &ip, uint16_t port);

    // Set callback for newly discovered nodes (for PEX integration)
    void set_node_callback(NodeCallback cb) { node_cb_ = std::move(cb); }

    // Called when a UDP packet is received on the DHT port
    void on_udp_data(const uint8_t *data, size_t len,
                     const std::string &remote_ip, uint16_t remote_port);

    // Get the list of nodes in compact format (for get_peers response)
    std::vector<DhtCompactNode> find_closest_nodes(const DhtNodeId &target, int count = 8);

    // Get routing table size
    size_t routing_table_size() const;

    // ConnectionHandler interface (for UdpAcceptor)
    void on_connected(net::Connection *conn) override;
    void on_error(net::Connection *conn) override;
    void on_read(net::Connection *conn) override;
    void on_write(net::Connection *conn) override;
    void on_close(net::Connection *conn) override;

private:
    // Routing table management
    void init_routing_table();
    int bucket_index(const DhtNodeId &id) const;
    void update_bucket(const DhtCompactNode &node);
    void refresh_bucket(int index);

    // RPC methods
    void send_ping(const std::string &ip, uint16_t port);
    void send_find_node(const std::string &ip, uint16_t port, const DhtNodeId &target);
    void send_get_peers(const std::string &ip, uint16_t port, const std::vector<uint8_t> &info_hash);
    void send_announce_peer(const std::string &ip, uint16_t port,
                            const std::vector<uint8_t> &info_hash,
                            uint16_t port_, const std::string &token);

    // Response handlers
    void handle_ping_response(const std::string &ip, uint16_t port,
                              const DhtNodeId &id);
    void handle_find_node_response(const std::string &ip, uint16_t port,
                                   const DhtNodeId &id,
                                   const std::vector<DhtCompactNode> &nodes);
    void handle_get_peers_response(const std::string &ip, uint16_t port,
                                   const DhtNodeId &id,
                                   const std::string &token,
                                   const std::vector<DhtCompactNode> &nodes,
                                   const std::vector<PeerAddress> &peers);

    // Request handling (when other nodes query us)
    void handle_ping_query(const std::string &ip, uint16_t port,
                           const std::string &transaction_id, const DhtNodeId &sender_id);
    void handle_find_node_query(const std::string &ip, uint16_t port,
                                const std::string &transaction_id,
                                const DhtNodeId &sender_id, const DhtNodeId &target);
    void handle_get_peers_query(const std::string &ip, uint16_t port,
                                const std::string &transaction_id,
                                const DhtNodeId &sender_id,
                                const std::vector<uint8_t> &info_hash);
    void handle_announce_peer_query(const std::string &ip, uint16_t port,
                                    const std::string &transaction_id,
                                    const DhtNodeId &sender_id,
                                    const std::vector<uint8_t> &info_hash,
                                    uint16_t peer_port, const std::string &token);

    // Message parsing and construction
    bool parse_dht_message(const uint8_t *data, size_t len,
                           std::string &transaction_id,
                           std::string &msg_type,  // "q" or "r" or "e"
                           std::string &query_type,
                           DhtNodeId &sender_id);
    std::vector<uint8_t> build_response(const std::string &transaction_id,
                                        const std::string &response_data);
    std::vector<uint8_t> build_error(const std::string &transaction_id,
                                     int code, const std::string &msg);

    // Utility
    void send_udp(const std::string &ip, uint16_t port,
                  const uint8_t *data, size_t len);
    DhtNodeId generate_node_id();
    static DhtNodeId xor_distance(const DhtNodeId &a, const DhtNodeId &b);
    static int leading_zeros(const DhtNodeId &id);
    static std::string node_id_to_string(const DhtNodeId &id);
    std::string next_tid();

    // Bootstrap
    void bootstrap();
    void bootstrap_done_callback(const std::string &ip, uint16_t port,
                                 const DhtNodeId &id);

    // Periodic maintenance
    void periodic_refresh();

private:
    bool running_ = false;
    int32_t port_ = 0;

    net::UdpAcceptor *acceptor_ = nullptr;
    net::EventLoop *ev_loop_ = nullptr;
    timer::TimerManager *timer_manager_ = nullptr;

    DhtNodeId node_id_;
    std::string external_ip_;
    NatConfig config_;

    // Routing table (160 buckets, one per bit position)
    std::vector<DhtBucket> buckets_;

    // Pending RPC queries: transaction_id -> {callback, expire_time}
    struct PendingQuery
    {
        std::function<void()> callback;
        int64_t expire_time_ms;
    };
    std::unordered_map<std::string, PendingQuery> pending_queries_;
    uint32_t next_transaction_id_ = 0;

    // Token storage for announce_peer (map: ip -> {token, expire})
    std::unordered_map<std::string, std::pair<std::string, int64_t>> tokens_;

    // Announce peer storage: info_hash_hex -> list of {ip, port, expire}
    struct StoredPeer
    {
        std::string ip;
        uint16_t port;
        int64_t expire_ms;
    };
    std::unordered_map<std::string, std::vector<StoredPeer>> peer_store_;

    // Active get_peers lookups
    struct ActiveLookup
    {
        PeerCallback callback;
        std::vector<DhtNodeId> queried;  // nodes we've already queried
        int queries_sent = 0;
        int responses_received = 0;
    };
    std::unordered_map<std::string, ActiveLookup> active_lookups_;

    // Known bootstrap nodes
    static constexpr const char *BOOTSTRAP_NODES[][2] = {
        {"router.bittorrent.com", "6881"},
        {"dht.transmissionbt.com", "6881"},
        {"dht.aelitis.com", "6881"},     // Vuze
        {"router.utorrent.com", "6881"},
        {"betadoctor.yellowmoz.com", "6881"},
    };
    static constexpr size_t BOOTSTRAP_NODE_COUNT = 5;

    PeerCallback peer_callback_;
    NodeCallback node_cb_;
    timer::Timer *refresh_timer_ = nullptr;
};

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_NAT_DHT_NODE_H__
