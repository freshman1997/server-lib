#include "nat/dht_node.h"
#include "nat/nat_config.h"
#include "structure/bencoding.h"
#include "net/acceptor/udp_acceptor.h"
#include "net/socket/socket.h"
#include "net/socket/inet_address.h"
#include "net/connection/connection.h"
#include "timer/timer.h"
#include "timer/timer_task.h"
#include "buffer/buffer.h"
#include "buffer/pool.h"
#include "utils.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include <cstring>
#include <algorithm>
#include <random>
#include <chrono>

namespace yuan::net::bit_torrent
{

// Local timer task adapter
class DhtTimerTask : public timer::TimerTask
{
public:
    explicit DhtTimerTask(std::function<void(timer::Timer *)> fn) : fn_(std::move(fn)) {}
    void on_timer(timer::Timer *t) override { fn_(t); }
private:
    std::function<void(timer::Timer *)> fn_;
};

// ===== DhtCompactNode =====

std::string DhtCompactNode::ip_string() const
{
    char buf[INET_ADDRSTRLEN];
    uint32_t net_ip_val = ip;
    inet_ntop(AF_INET, &net_ip_val, buf, sizeof(buf));
    return buf;
}

DhtCompactNode DhtCompactNode::from_compact(const uint8_t *data)
{
    DhtCompactNode node;
    std::memcpy(node.id.data(), data, 20);
    node.ip = (static_cast<uint32_t>(data[20]) << 24) |
              (static_cast<uint32_t>(data[21]) << 16) |
              (static_cast<uint32_t>(data[22]) << 8) |
              static_cast<uint32_t>(data[23]);
    node.port = (static_cast<uint16_t>(data[24]) << 8) | data[25];
    return node;
}

std::vector<uint8_t> DhtCompactNode::to_compact() const
{
    std::vector<uint8_t> data(26);
    std::memcpy(data.data(), id.data(), 20);
    data[20] = (ip >> 24) & 0xFF;
    data[21] = (ip >> 16) & 0xFF;
    data[22] = (ip >> 8) & 0xFF;
    data[23] = ip & 0xFF;
    data[24] = (port >> 8) & 0xFF;
    data[25] = port & 0xFF;
    return data;
}

// ===== DhtBucket =====

bool DhtBucket::contains(const DhtNodeId &id) const
{
    for (const auto &node : nodes)
    {
        if (node.id == id) return true;
    }
    return false;
}

void DhtBucket::add(const DhtCompactNode &node)
{
    // If already in bucket, move to tail (most recently seen)
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
    {
        if (it->id == node.id)
        {
            nodes.erase(it);
            nodes.push_back(node);
            return;
        }
    }

    if (full())
    {
        // Add to replacement cache
        replacement_cache.push_back(node);
        if (replacement_cache.size() > K)
            replacement_cache.erase(replacement_cache.begin());
    }
    else
    {
        nodes.push_back(node);
    }
}

void DhtBucket::remove(const DhtNodeId &id)
{
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
    {
        if (it->id == id)
        {
            nodes.erase(it);
            // Promote from replacement cache
            if (!replacement_cache.empty())
            {
                nodes.push_back(replacement_cache.front());
                replacement_cache.erase(replacement_cache.begin());
            }
            return;
        }
    }
}

void DhtBucket::touch(const DhtNodeId &id)
{
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
    {
        if (it->id == id)
        {
            DhtCompactNode node = *it;
            nodes.erase(it);
            nodes.push_back(node);
            return;
        }
    }
}

// ===== DhtNode =====

DhtNode::DhtNode()
    : running_(false),
      port_(0),
      acceptor_(nullptr),
      ev_loop_(nullptr),
      timer_manager_(nullptr),
      refresh_timer_(nullptr),
      next_transaction_id_(0)
{
    init_routing_table();
}

DhtNode::~DhtNode()
{
    stop();
}

void DhtNode::init_routing_table()
{
    buckets_.resize(160);
    for (size_t i = 0; i < 160; i++)
    {
        buckets_[i].prefix.fill(0);
        // Set the i-th bit from the left
        buckets_[i].prefix[i / 8] |= (1 << (7 - (i % 8)));
    }
}

bool DhtNode::start(const NatConfig &config,
                     net::EventLoop *loop,
                     timer::TimerManager *timer_mgr,
                     const std::string &external_ip)
{
    if (running_) return true;

    config_ = config;
    ev_loop_ = loop;
    timer_manager_ = timer_mgr;
    external_ip_ = external_ip;

    // Generate node ID
    node_id_ = generate_node_id();

    int32_t bind_port = config.dht_port > 0 ? config.dht_port : config.listen_port;

    auto *sock = new net::Socket("", bind_port, true);
    if (!sock->valid())
    {
        delete sock;
        return false;
    }

    sock->set_reuse(true);
    sock->set_none_block(true);

    acceptor_ = new net::UdpAcceptor(sock, timer_mgr);
    if (!acceptor_->listen())
    {
        delete acceptor_;
        acceptor_ = nullptr;
        delete sock;
        return false;
    }

    acceptor_->set_connection_handler(this);
    acceptor_->set_event_handler(ev_loop_);
    ev_loop_->update_channel(acceptor_->get_channel());

    port_ = bind_port;
    running_ = true;

    // Start bootstrap
    bootstrap();

    // Start periodic refresh
    if (timer_manager_)
    {
        refresh_timer_ = timer_manager_->interval(
            config.dht_refresh_interval_s * 1000,
            config.dht_refresh_interval_s * 1000,
            new DhtTimerTask([this](timer::Timer *) { periodic_refresh(); }),
            -1);
    }

    return true;
}

void DhtNode::stop()
{
    if (!running_) return;
    running_ = false;

    if (refresh_timer_)
    {
        refresh_timer_->cancel();
        refresh_timer_ = nullptr;
    }

    if (acceptor_)
    {
        acceptor_->close();
        delete acceptor_;
        acceptor_ = nullptr;
    }

    pending_queries_.clear();
    active_lookups_.clear();
    peer_store_.clear();
}

void DhtNode::announce(const std::vector<uint8_t> &info_hash, uint16_t port)
{
    if (!running_) return;

    // Find closest nodes to the info_hash
    auto closest = find_closest_nodes(DhtNodeId{}, 8);

    // Convert info_hash to DhtNodeId
    DhtNodeId target;
    std::memcpy(target.data(), info_hash.data(), 20);

    closest = find_closest_nodes(target, 8);

    for (const auto &node : closest)
    {
        // We need a token first, so send get_peers
        send_get_peers(node.ip_string(), node.port, info_hash);
    }
}

void DhtNode::get_peers(const std::vector<uint8_t> &info_hash, PeerCallback cb)
{
    if (!running_) return;

    DhtNodeId target;
    std::memcpy(target.data(), info_hash.data(), 20);

    std::string key(reinterpret_cast<const char *>(info_hash.data()), 20);

    ActiveLookup lookup;
    lookup.callback = std::move(cb);
    lookup.queries_sent = 0;
    lookup.responses_received = 0;

    auto closest = find_closest_nodes(target, 8);
    for (const auto &node : closest)
    {
        std::string nkey = node.ip_string() + ":" + std::to_string(node.port);
        lookup.queried.push_back(node.id);
        send_get_peers(node.ip_string(), node.port, info_hash);
        lookup.queries_sent++;
    }

    active_lookups_[key] = std::move(lookup);
}

void DhtNode::add_node(const std::string &ip, uint16_t port)
{
    DhtCompactNode node;
    node.id = {}; // unknown ID for now
    uint32_t net_ip = 0;
    inet_pton(AF_INET, ip.c_str(), &net_ip);
    node.ip = net_ip;
    node.port = port;
    update_bucket(node);
}

// ===== Routing Table =====

int DhtNode::bucket_index(const DhtNodeId &id) const
{
    DhtNodeId dist = xor_distance(node_id_, id);
    return leading_zeros(dist);
}

void DhtNode::update_bucket(const DhtCompactNode &node)
{
    if (node.id == DhtNodeId{}) return; // skip unknown IDs

    int idx = bucket_index(node.id);
    if (idx >= 0 && idx < static_cast<int>(buckets_.size()))
    {
        buckets_[idx].add(node);
    }
}

void DhtNode::refresh_bucket(int index)
{
    if (index < 0 || index >= static_cast<int>(buckets_.size())) return;

    // Generate a random ID in this bucket's range
    DhtNodeId target;
    std::memcpy(target.data(), buckets_[index].prefix.data(), 20);

    // Find the closest node in the bucket to ping
    if (!buckets_[index].nodes.empty())
    {
        auto &node = buckets_[index].nodes.back();
        send_find_node(node.ip_string(), node.port, target);
    }
}

void DhtNode::periodic_refresh()
{
    if (!running_) return;

    // Refresh least-recently-refreshed bucket
    // Simplified: refresh buckets that haven't been updated
    for (size_t i = 0; i < buckets_.size(); i++)
    {
        if (!buckets_[i].nodes.empty())
        {
            refresh_bucket(static_cast<int>(i));
            break; // refresh one bucket per interval
        }
    }
}

std::vector<DhtCompactNode> DhtNode::find_closest_nodes(const DhtNodeId &target, int count)
{
    // Find the bucket that would contain target
    int idx = leading_zeros(xor_distance(node_id_, target));
    if (idx >= static_cast<int>(buckets_.size())) idx = static_cast<int>(buckets_.size()) - 1;

    // Collect nodes from closest buckets
    std::vector<DhtCompactNode> candidates;

    // Add nodes from the target bucket first
    for (const auto &node : buckets_[idx].nodes)
        candidates.push_back(node);

    // Expand to adjacent buckets
    for (int d = 1; static_cast<int>(candidates.size()) < count * 2; d++)
    {
        if (idx - d >= 0)
            for (const auto &node : buckets_[idx - d].nodes)
                candidates.push_back(node);
        if (idx + d < static_cast<int>(buckets_.size()))
            for (const auto &node : buckets_[idx + d].nodes)
                candidates.push_back(node);
    }

    // Sort by XOR distance to target
    DhtNodeId self_dist = xor_distance(node_id_, target);
    std::sort(candidates.begin(), candidates.end(),
              [&target](const DhtCompactNode &a, const DhtCompactNode &b)
    {
        return xor_distance(a.id, target) < xor_distance(b.id, target);
    });

    // Return top `count`
    if (static_cast<int>(candidates.size()) > count)
        candidates.resize(count);

    return candidates;
}

size_t DhtNode::routing_table_size() const
{
    size_t total = 0;
    for (const auto &b : buckets_)
        total += b.nodes.size();
    return total;
}

// ===== RPC =====

void DhtNode::send_ping(const std::string &ip, uint16_t port)
{
    std::string tid = next_tid();

    DicttionaryData dict;
    dict.add("t", new StringData(tid));
    dict.add("y", new StringData("q"));
    dict.add("q", new StringData("ping"));
    auto *a = new DicttionaryData;
    a->add("id", new StringData(node_id_to_string(node_id_)));
    dict.add("a", a);

    std::string msg = BencodingDataConverter::encode(&dict);
    send_udp(ip, port, reinterpret_cast<const uint8_t *>(msg.data()), msg.size());

    PendingQuery pq;
    pq.callback = [this, ip, port]() {};
    auto now = std::chrono::steady_clock::now();
    pq.expire_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() + 15000;
    pending_queries_[tid] = pq;
}

void DhtNode::send_find_node(const std::string &ip, uint16_t port, const DhtNodeId &target)
{
    std::string tid = next_tid();

    DicttionaryData dict;
    dict.add("t", new StringData(tid));
    dict.add("y", new StringData("q"));
    dict.add("q", new StringData("find_node"));
    auto *a = new DicttionaryData;
    a->add("id", new StringData(node_id_to_string(node_id_)));
    a->add("target", new StringData(node_id_to_string(target)));
    dict.add("a", a);

    std::string msg = BencodingDataConverter::encode(&dict);
    send_udp(ip, port, reinterpret_cast<const uint8_t *>(msg.data()), msg.size());

    PendingQuery pq;
    pq.expire_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() + 15000;
    pending_queries_[tid] = pq;
}

void DhtNode::send_get_peers(const std::string &ip, uint16_t port,
                              const std::vector<uint8_t> &info_hash)
{
    std::string tid = next_tid();

    DicttionaryData dict;
    dict.add("t", new StringData(tid));
    dict.add("y", new StringData("q"));
    dict.add("q", new StringData("get_peers"));
    auto *a = new DicttionaryData;
    a->add("id", new StringData(node_id_to_string(node_id_)));
    a->add("info_hash", new StringData(
        reinterpret_cast<const char *>(info_hash.data()),
        reinterpret_cast<const char *>(info_hash.data() + 20)));
    dict.add("a", a);

    std::string msg = BencodingDataConverter::encode(&dict);
    send_udp(ip, port, reinterpret_cast<const uint8_t *>(msg.data()), msg.size());

    PendingQuery pq;
    pq.expire_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() + 15000;
    pending_queries_[tid] = pq;
}

void DhtNode::send_announce_peer(const std::string &ip, uint16_t port,
                                   const std::vector<uint8_t> &info_hash,
                                   uint16_t peer_port, const std::string &token)
{
    std::string tid = next_tid();

    DicttionaryData dict;
    dict.add("t", new StringData(tid));
    dict.add("y", new StringData("q"));
    dict.add("q", new StringData("announce_peer"));
    auto *a = new DicttionaryData;
    a->add("id", new StringData(node_id_to_string(node_id_)));
    a->add("info_hash", new StringData(
        reinterpret_cast<const char *>(info_hash.data()),
        reinterpret_cast<const char *>(info_hash.data() + 20)));
    a->add("port", new IntegerData(peer_port));
    a->add("token", new StringData(token));
    dict.add("a", a);

    std::string msg = BencodingDataConverter::encode(&dict);
    send_udp(ip, port, reinterpret_cast<const uint8_t *>(msg.data()), msg.size());
}

// ===== Bootstrap =====

void DhtNode::bootstrap()
{
    for (size_t i = 0; i < BOOTSTRAP_NODE_COUNT; i++)
    {
        // Resolve hostname and ping
        // For simplicity, we use the known IPs for popular bootstrap nodes
        // In production, use DNS resolution
        std::string host = BOOTSTRAP_NODES[i][0];
        uint16_t port = static_cast<uint16_t>(std::atoi(BOOTSTRAP_NODES[i][1]));

        std::string ip = net::InetAddress::get_address_by_host(host);
        if (!ip.empty())
        {
            send_ping(ip, port);
        }
    }
}

// ===== Response Handlers =====

void DhtNode::handle_ping_response(const std::string &ip, uint16_t port,
                                    const DhtNodeId &id)
{
    DhtCompactNode node;
    node.id = id;
    uint32_t net_ip = 0;
    inet_pton(AF_INET, ip.c_str(), &net_ip);
    node.ip = net_ip;
    node.port = port;
    update_bucket(node);

    // After bootstrap ping, do find_node to populate routing table
    DhtNodeId target = node_id_; // find nodes close to ourselves
    send_find_node(ip, port, target);
}

void DhtNode::handle_find_node_response(const std::string &ip, uint16_t port,
                                         const DhtNodeId &id,
                                         const std::vector<DhtCompactNode> &nodes)
{
    // Update routing table with the responder
    DhtCompactNode responder;
    responder.id = id;
    uint32_t net_ip = 0;
    inet_pton(AF_INET, ip.c_str(), &net_ip);
    responder.ip = net_ip;
    responder.port = port;
    update_bucket(responder);

    // Add discovered nodes to routing table
    for (const auto &node : nodes)
    {
        update_bucket(node);
    }

    if (node_cb_)
    {
        for (const auto &node : nodes)
            node_cb_(node);
    }
}

void DhtNode::handle_get_peers_response(const std::string &ip, uint16_t port,
                                         const DhtNodeId &id,
                                         const std::string &token,
                                         const std::vector<DhtCompactNode> &nodes,
                                         const std::vector<PeerAddress> &peers)
{
    // Update routing table
    DhtCompactNode responder;
    responder.id = id;
    uint32_t net_ip = 0;
    inet_pton(AF_INET, ip.c_str(), &net_ip);
    responder.ip = net_ip;
    responder.port = port;
    update_bucket(responder);

    // Store the token for this node (for announce_peer)
    std::string key = ip + ":" + std::to_string(port);
    auto now = std::chrono::steady_clock::now();
    auto expire_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() + 600000; // 10 minutes
    tokens_[key] = {token, expire_ms};

    // Process discovered peers
    if (!peers.empty())
    {
        // Find the active lookup for this info_hash
        // We'll match by checking if any lookup has this ip as a queried node
        if (peer_callback_)
        {
            peer_callback_(peers);
        }
    }

    // Continue querying closer nodes if we got nodes instead of peers
    if (!nodes.empty())
    {
        for (const auto &node : nodes)
        {
            update_bucket(node);
        }
    }

    // Add nodes to routing table
    for (const auto &node : nodes)
    {
        update_bucket(node);
    }
}

// ===== Query Handlers (when other nodes query us) =====

void DhtNode::handle_ping_query(const std::string &ip, uint16_t port,
                                 const std::string &transaction_id, const DhtNodeId &sender_id)
{
    // Update routing table
    DhtCompactNode node;
    node.id = sender_id;
    uint32_t net_ip = 0;
    inet_pton(AF_INET, ip.c_str(), &net_ip);
    node.ip = net_ip;
    node.port = port;
    update_bucket(node);

    // Build response: {"t":"<tid>", "y":"r", "r":{"id":"<our_id>"}}
    DicttionaryData resp;
    resp.add("t", new StringData(transaction_id));
    resp.add("y", new StringData("r"));
    auto *r = new DicttionaryData;
    r->add("id", new StringData(node_id_to_string(node_id_)));
    resp.add("r", r);

    std::string msg = BencodingDataConverter::encode(&resp);
    send_udp(ip, port, reinterpret_cast<const uint8_t *>(msg.data()), msg.size());
}

void DhtNode::handle_find_node_query(const std::string &ip, uint16_t port,
                                      const std::string &transaction_id,
                                      const DhtNodeId &sender_id, const DhtNodeId &target)
{
    // Update routing table
    DhtCompactNode node;
    node.id = sender_id;
    uint32_t net_ip = 0;
    inet_pton(AF_INET, ip.c_str(), &net_ip);
    node.ip = net_ip;
    node.port = port;
    update_bucket(node);

    // Find closest nodes
    auto closest = find_closest_nodes(target, 8);

    // Build compact nodes string
    std::string compact_nodes;
    for (const auto &n : closest)
    {
        auto compact = n.to_compact();
        compact_nodes.append(reinterpret_cast<const char *>(compact.data()), compact.size());
    }

    // Response
    DicttionaryData resp;
    resp.add("t", new StringData(transaction_id));
    resp.add("y", new StringData("r"));
    auto *r = new DicttionaryData;
    r->add("id", new StringData(node_id_to_string(node_id_)));
    r->add("nodes", new StringData(compact_nodes));
    resp.add("r", r);

    std::string msg = BencodingDataConverter::encode(&resp);
    send_udp(ip, port, reinterpret_cast<const uint8_t *>(msg.data()), msg.size());
}

void DhtNode::handle_get_peers_query(const std::string &ip, uint16_t port,
                                      const std::string &transaction_id,
                                      const DhtNodeId &sender_id,
                                      const std::vector<uint8_t> &info_hash)
{
    // Update routing table
    DhtCompactNode node;
    node.id = sender_id;
    uint32_t net_ip = 0;
    inet_pton(AF_INET, ip.c_str(), &net_ip);
    node.ip = net_ip;
    node.port = port;
    update_bucket(node);

    // Generate a token for this node
    std::string token_key = ip + ":" + std::to_string(port);
    std::string token = std::to_string(next_transaction_id_++);
    auto now = std::chrono::steady_clock::now();
    auto expire_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() + 600000;
    tokens_[token_key] = {token, expire_ms};

    // Check if we have peers for this info_hash
    std::string ih_key(reinterpret_cast<const char *>(info_hash.data()), 20);
    std::string compact_peers;

    auto it = peer_store_.find(ih_key);
    if (it != peer_store_.end() && !it->second.empty())
    {
        for (const auto &p : it->second)
        {
            uint8_t compact[6];
            inet_pton(AF_INET, p.ip.c_str(), compact);
            compact[4] = (p.port >> 8) & 0xFF;
            compact[5] = p.port & 0xFF;
            compact_peers.append(reinterpret_cast<const char *>(compact), 6);
        }
    }

    // Find closest nodes as well
    DhtNodeId target;
    std::memcpy(target.data(), info_hash.data(), 20);
    auto closest = find_closest_nodes(target, 8);
    std::string compact_nodes;
    for (const auto &n : closest)
    {
        auto compact = n.to_compact();
        compact_nodes.append(reinterpret_cast<const char *>(compact.data()), compact.size());
    }

    // Build response
    DicttionaryData resp;
    resp.add("t", new StringData(transaction_id));
    resp.add("y", new StringData("r"));
    auto *r = new DicttionaryData;
    r->add("id", new StringData(node_id_to_string(node_id_)));
    r->add("token", new StringData(token));

    if (!compact_peers.empty())
        r->add("values", new StringData(compact_peers));

    if (!compact_nodes.empty())
        r->add("nodes", new StringData(compact_nodes));

    resp.add("r", r);

    std::string msg = BencodingDataConverter::encode(&resp);
    send_udp(ip, port, reinterpret_cast<const uint8_t *>(msg.data()), msg.size());
}

void DhtNode::handle_announce_peer_query(const std::string &ip, uint16_t port,
                                          const std::string &transaction_id,
                                          const DhtNodeId &sender_id,
                                          const std::vector<uint8_t> &info_hash,
                                          uint16_t peer_port, const std::string &token)
{
    // Verify token
    std::string token_key = ip + ":" + std::to_string(port);
    auto tit = tokens_.find(token_key);
    if (tit == tokens_.end() || tit->second.first != token)
    {
        // Invalid token - send error
        DicttionaryData resp;
        resp.add("t", new StringData(transaction_id));
        resp.add("y", new StringData("e"));
        auto *err_list = new Listdata;
        err_list->push(new IntegerData(201));
        err_list->push(new StringData("Bad announce peer token"));
        resp.add("e", err_list);

        std::string msg = BencodingDataConverter::encode(&resp);
        send_udp(ip, port, reinterpret_cast<const uint8_t *>(msg.data()), msg.size());
        return;
    }

    // Store the peer
    std::string ih_key(reinterpret_cast<const char *>(info_hash.data()), 20);
    StoredPeer sp;
    sp.ip = ip;
    sp.port = peer_port;
    auto now = std::chrono::steady_clock::now();
    sp.expire_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() + 3600000; // 1 hour
    peer_store_[ih_key].push_back(sp);

    // Send response
    DicttionaryData resp;
    resp.add("t", new StringData(transaction_id));
    resp.add("y", new StringData("r"));
    auto *r = new DicttionaryData;
    r->add("id", new StringData(node_id_to_string(node_id_)));
    resp.add("r", r);

    std::string msg = BencodingDataConverter::encode(&resp);
    send_udp(ip, port, reinterpret_cast<const uint8_t *>(msg.data()), msg.size());
}

// ===== Message Parsing =====

bool DhtNode::parse_dht_message(const uint8_t *data, size_t len,
                                 std::string &transaction_id,
                                 std::string &msg_type,
                                 std::string &query_type,
                                 DhtNodeId &sender_id)
{
    std::string str(reinterpret_cast<const char *>(data), len);
    auto *parsed = BencodingDataConverter::parse(str);
    if (!parsed || parsed->type_ != DataType::dictionary_)
    {
        delete parsed;
        return false;
    }

    auto *dict = static_cast<DicttionaryData *>(parsed);

    // Get transaction ID
    auto *t = dict->get_val("t");
    if (t && t->type_ == DataType::string_)
        transaction_id = static_cast<StringData *>(t)->get_data();

    // Get message type ("q" or "r" or "e")
    auto *y = dict->get_val("y");
    if (y && y->type_ == DataType::string_)
        msg_type = static_cast<StringData *>(y)->get_data();

    // Get query type (if query)
    auto *q = dict->get_val("q");
    if (q && q->type_ == DataType::string_)
        query_type = static_cast<StringData *>(q)->get_data();

    // Get sender ID (from "a" for queries or "r" for responses)
    auto *a = dict->get_val("a");
    auto *r = dict->get_val("r");

    BaseData *id_data = a ? a : r;
    if (id_data && id_data->type_ == DataType::dictionary_)
    {
        auto *id_dict = static_cast<DicttionaryData *>(id_data);
        auto *id_val = id_dict->get_val("id");
        if (id_val && id_val->type_ == DataType::string_)
        {
            const std::string &id_str = static_cast<StringData *>(id_val)->get_data();
            if (id_str.size() >= 20)
                std::memcpy(sender_id.data(), id_str.data(), 20);
        }
    }

    delete parsed;
    return true;
}

// ===== UDP receive =====

void DhtNode::on_udp_data(const uint8_t *data, size_t len,
                           const std::string &remote_ip, uint16_t remote_port)
{
    std::string tid, msg_type, query_type;
    DhtNodeId sender_id;

    if (!parse_dht_message(data, len, tid, msg_type, query_type, sender_id))
        return;

    if (msg_type == "q")
    {
        // Incoming query
        if (query_type == "ping")
        {
            handle_ping_query(remote_ip, remote_port, tid, sender_id);
        }
        else if (query_type == "find_node")
        {
            // Extract target from the "a" dict
            std::string str(reinterpret_cast<const char *>(data), len);
            auto *parsed = BencodingDataConverter::parse(str);
            if (parsed && parsed->type_ == DataType::dictionary_)
            {
                auto *dict = static_cast<DicttionaryData *>(parsed);
                auto *a = dict->get_val("a");
                if (a && a->type_ == DataType::dictionary_)
                {
                    auto *a_dict = static_cast<DicttionaryData *>(a);
                    auto *target = a_dict->get_val("target");
                    if (target && target->type_ == DataType::string_)
                    {
                        const std::string &target_str = static_cast<StringData *>(target)->get_data();
                        DhtNodeId target_id;
                        std::memcpy(target_id.data(), target_str.data(), std::min(target_str.size(), size_t(20)));
                        handle_find_node_query(remote_ip, remote_port, tid, sender_id, target_id);
                    }
                }
            }
            delete parsed;
        }
        else if (query_type == "get_peers")
        {
            std::string str(reinterpret_cast<const char *>(data), len);
            auto *parsed = BencodingDataConverter::parse(str);
            if (parsed && parsed->type_ == DataType::dictionary_)
            {
                auto *dict = static_cast<DicttionaryData *>(parsed);
                auto *a = dict->get_val("a");
                if (a && a->type_ == DataType::dictionary_)
                {
                    auto *a_dict = static_cast<DicttionaryData *>(a);
                    auto *ih = a_dict->get_val("info_hash");
                    if (ih && ih->type_ == DataType::string_)
                    {
                        const std::string &ih_str = static_cast<StringData *>(ih)->get_data();
                        std::vector<uint8_t> info_hash(ih_str.begin(), ih_str.end());
                        handle_get_peers_query(remote_ip, remote_port, tid, sender_id, info_hash);
                    }
                }
            }
            delete parsed;
        }
        else if (query_type == "announce_peer")
        {
            std::string str(reinterpret_cast<const char *>(data), len);
            auto *parsed = BencodingDataConverter::parse(str);
            if (parsed && parsed->type_ == DataType::dictionary_)
            {
                auto *dict = static_cast<DicttionaryData *>(parsed);
                auto *a = dict->get_val("a");
                if (a && a->type_ == DataType::dictionary_)
                {
                    auto *a_dict = static_cast<DicttionaryData *>(a);
                    auto *ih = a_dict->get_val("info_hash");
                    auto *port_val = a_dict->get_val("port");
                    auto *token_val = a_dict->get_val("token");

                    std::vector<uint8_t> info_hash;
                    uint16_t peer_port = 0;
                    std::string token;

                    if (ih && ih->type_ == DataType::string_)
                    {
                        const std::string &ih_str = static_cast<StringData *>(ih)->get_data();
                        info_hash.assign(ih_str.begin(), ih_str.end());
                    }
                    if (port_val && port_val->type_ == DataType::integer_)
                    {
                        peer_port = static_cast<uint16_t>(static_cast<IntegerData *>(port_val)->get_data());
                    }
                    if (token_val && token_val->type_ == DataType::string_)
                    {
                        token = static_cast<StringData *>(token_val)->get_data();
                    }

                    if (!info_hash.empty())
                    {
                        handle_announce_peer_query(remote_ip, remote_port, tid,
                                                    sender_id, info_hash, peer_port, token);
                    }
                }
            }
            delete parsed;
        }
    }
    else if (msg_type == "r")
    {
        // Response to our query
        pending_queries_.erase(tid);

        std::string str(reinterpret_cast<const char *>(data), len);
        auto *parsed = BencodingDataConverter::parse(str);
        if (!parsed || parsed->type_ != DataType::dictionary_)
        {
            delete parsed;
            return;
        }

        auto *dict = static_cast<DicttionaryData *>(parsed);
        auto *r = dict->get_val("r");
        if (!r || r->type_ != DataType::dictionary_)
        {
            delete parsed;
            return;
        }

        auto *r_dict = static_cast<DicttionaryData *>(r);

        // Check if this has "values" (get_peers response with peers) or "nodes" (find_node/get_peers with nodes)
        auto *values = r_dict->get_val("values");
        auto *nodes_val = r_dict->get_val("nodes");
        auto *token_val = r_dict->get_val("token");

        std::string token;
        if (token_val && token_val->type_ == DataType::string_)
            token = static_cast<StringData *>(token_val)->get_data();

        std::vector<DhtCompactNode> nodes;
        if (nodes_val && nodes_val->type_ == DataType::string_)
        {
            const std::string &nodes_str = static_cast<StringData *>(nodes_val)->get_data();
            size_t count = nodes_str.size() / 26;
            for (size_t i = 0; i < count; i++)
            {
                nodes.push_back(DhtCompactNode::from_compact(
                    reinterpret_cast<const uint8_t *>(nodes_str.data()) + i * 26));
            }
        }

        std::vector<PeerAddress> peers;
        if (values && values->type_ == DataType::string_)
        {
            const std::string &values_str = static_cast<StringData *>(values)->get_data();
            size_t count = values_str.size() / 6;
            for (size_t i = 0; i < count; i++)
            {
                const uint8_t *p = reinterpret_cast<const uint8_t *>(values_str.data()) + i * 6;
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, p, ip_str, sizeof(ip_str));
                uint16_t port = (static_cast<uint16_t>(p[4]) << 8) | p[5];
                peers.push_back({ip_str, port});
            }
        }

        if (!peers.empty())
        {
            handle_get_peers_response(remote_ip, remote_port, sender_id, token, nodes, peers);
        }
        else if (!nodes.empty())
        {
            handle_find_node_response(remote_ip, remote_port, sender_id, nodes);
        }
        else
        {
            handle_ping_response(remote_ip, remote_port, sender_id);
        }

        delete parsed;
    }

    // Clean up expired pending queries
    auto now = std::chrono::steady_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    for (auto it = pending_queries_.begin(); it != pending_queries_.end();)
    {
        if (it->second.expire_time_ms < now_ms)
            it = pending_queries_.erase(it);
        else
            ++it;
    }

    // Clean up expired tokens and peer store
    for (auto it = tokens_.begin(); it != tokens_.end();)
    {
        if (it->second.second < now_ms)
            it = tokens_.erase(it);
        else
            ++it;
    }

    for (auto it = peer_store_.begin(); it != peer_store_.end();)
    {
        auto &peer_list = it->second;
        for (auto pit = peer_list.begin(); pit != peer_list.end();)
        {
            if (pit->expire_ms < now_ms)
                pit = peer_list.erase(pit);
            else
                ++pit;
        }
        if (peer_list.empty())
            it = peer_store_.erase(it);
        else
            ++it;
    }
}

// ===== Utility =====

void DhtNode::send_udp(const std::string &ip, uint16_t port,
                        const uint8_t *data, size_t len)
{
    if (!acceptor_) return;

    auto *buf = buffer::BufferedPool::get_instance()->allocate(len);
    buf->write_string(reinterpret_cast<const char *>(data), len);

    net::InetAddress addr(ip, port);
    acceptor_->send_to(addr, buf);
}

DhtNodeId DhtNode::generate_node_id()
{
    // Generate a random node ID (in production, use a secure RNG)
    DhtNodeId id;
    std::random_device rd;
    for (size_t i = 0; i < 20; i++)
        id[i] = static_cast<uint8_t>(rd());

    return id;
}

DhtNodeId DhtNode::xor_distance(const DhtNodeId &a, const DhtNodeId &b)
{
    DhtNodeId result;
    for (size_t i = 0; i < 20; i++)
        result[i] = a[i] ^ b[i];
    return result;
}

int DhtNode::leading_zeros(const DhtNodeId &id)
{
    int zeros = 0;
    for (size_t i = 0; i < 20; i++)
    {
        if (id[i] == 0)
        {
            zeros += 8;
        }
        else
        {
            uint8_t byte = id[i];
            while ((byte & 0x80) == 0)
            {
                zeros++;
                byte <<= 1;
            }
            break;
        }
    }
    return std::min(zeros, 159);
}

std::string DhtNode::node_id_to_string(const DhtNodeId &id)
{
    return std::string(reinterpret_cast<const char *>(id.data()), 20);
}

std::string DhtNode::next_tid()
{
    return std::to_string(next_transaction_id_++);
}

// ConnectionHandler stubs (DHT uses UDP)
void DhtNode::on_connected(net::Connection *conn) {}
void DhtNode::on_error(net::Connection *conn) {}
void DhtNode::on_read(net::Connection *conn) {}
void DhtNode::on_write(net::Connection *conn) {}
void DhtNode::on_close(net::Connection *conn) {}

} // namespace yuan::net::bit_torrent
