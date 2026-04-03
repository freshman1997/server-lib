#ifndef __BIT_TORRENT_NAT_UTP_CONNECTION_H__
#define __BIT_TORRENT_NAT_UTP_CONNECTION_H__

// uTP (Micro Transport Protocol) - BEP 29
//
// uTP is a lightweight reliable transport protocol layered on UDP,
// designed for BitTorrent to minimize interference with other traffic.
// It uses LEDBAT congestion control and provides NAT hole punching
// capabilities when both peers know each other's external addresses.
//
// Header format (4 types):
//   ST_SYN    (0): <type:1><version:1><extension:1><conn_id:2><seq_nr:4><timestamp:4><timestamp_diff:4>  = 20 bytes
//   ST_STATE  (1): <type:1><version:1><extension:1><conn_id:2><seq_nr:4><ack_nr:4><timestamp:4><timestamp_diff:4><window_size:1>  = 20 bytes
//   ST_DATA   (2): same as ST_STATE + payload
//   ST_FIN    (3): same as ST_STATE (no payload)
//   ST_RESET  (4): <type:1><version:1><conn_id:2><seq_nr:4>  = 8 bytes

#include "peer_wire/peer_connection.h"
#include "net/acceptor/udp_acceptor.h"
#include "net/handler/connection_handler.h"
#include "net/connection/connection.h"
#include "timer/timer_manager.h"
#include <functional>
#include <unordered_map>
#include <queue>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace yuan::net::bit_torrent
{

struct NatConfig;
class PeerConnection;

// uTP packet types
enum class UtpType : uint8_t
{
    st_reset  = 0,
    st_state  = 1,
    st_syn    = 2,
    st_data   = 3,
    st_fin    = 4
};

// uTP packet header (common part)
#pragma pack(push, 1)
struct UtpHeader
{
    uint8_t  type;            // packet type (version in lower 4 bits for ST_STATE)
    uint8_t  version;         // protocol version (1)
    uint8_t  extension;       // extension type (0 = none)
    uint16_t conn_id;         // connection ID (network byte order)
    uint32_t seq_nr;          // sequence number (network byte order)
    uint32_t ack_nr;          // ack number (network byte order)
    uint32_t timestamp;       // microseconds (network byte order)
    uint32_t timestamp_diff;  // difference from previous (network byte order)
    uint8_t  window_size;     // receive window size (bytes / 1024)

    static constexpr uint8_t VERSION = 1;

    void set_type_ver(UtpType t, uint8_t ver = VERSION)
    {
        type = static_cast<uint8_t>(t) & 0x0F;
        version = (ver << 4) | (type & 0x0F);
    }

    UtpType get_type() const { return static_cast<UtpType>(type & 0x0F); }
    uint8_t get_version() const { return (type >> 4) & 0x0F; }

    void to_network_order()
    {
        conn_id = htons(conn_id);
        seq_nr = htonl(seq_nr);
        ack_nr = htonl(ack_nr);
        timestamp = htonl(timestamp);
        timestamp_diff = htonl(timestamp_diff);
    }

    void to_host_order()
    {
        conn_id = ntohs(conn_id);
        seq_nr = ntohl(seq_nr);
        ack_nr = ntohl(ack_nr);
        timestamp = ntohl(timestamp);
        timestamp_diff = ntohl(timestamp_diff);
    }
};
#pragma pack(pop)

static constexpr size_t UTP_HEADER_SIZE = sizeof(UtpHeader);  // 20 bytes
static constexpr size_t UTP_RESET_SIZE = 8;  // ST_RESET is shorter

// A single uTP connection, managing retransmission, flow control,
// and wrapping the BitTorrent peer wire protocol over uTP.
class UtpConnection : public net::ConnectionHandler, public timer::TimerTask
{
    friend class UtpManager;
public:
    enum class State
    {
        idle,
        syn_sent,       // We sent SYN, waiting for ST_STATE
        syn_recv,       // We received SYN, sent ST_STATE back
        connected,
        closed,
        error
    };

    using DataHandler = std::function<void(const uint8_t *data, size_t len)>;
    using StateChangeHandler = std::function<void(UtpConnection *, State)>;

    // Initiate outgoing uTP connection
    UtpConnection(const std::string &remote_ip, uint16_t remote_port,
                  const std::vector<uint8_t> &info_hash,
                  const std::string &peer_id,
                  uint32_t recv_conn_id,    // conn_id we send to them
                  uint32_t send_conn_id,    // conn_id we expect from them
                  net::UdpAcceptor *acceptor,
                  net::EventLoop *loop,
                  timer::TimerManager *timer_mgr);

    ~UtpConnection();

    // Called when we receive an incoming SYN (creates connection in syn_recv state)
    void accept_incoming(uint32_t send_conn_id, uint32_t recv_conn_id);

    // ConnectionHandler interface (not directly used, we use UDP)
    void on_connected(net::Connection *conn) override {}
    void on_error(net::Connection *conn) override {}
    void on_read(net::Connection *conn) override {}
    void on_write(net::Connection *conn) override {}
    void on_close(net::Connection *conn) override {}

    // TimerTask interface
    void on_timer(timer::Timer *timer) override;

    // Send data over uTP (queues and segments)
    void send_data(const uint8_t *data, size_t len);

    // Process a received uTP packet
    void on_packet_received(const uint8_t *data, size_t len);

    // Close connection gracefully
    void close();

    State get_state() const { return state_; }
    const std::string &get_remote_ip() const { return remote_ip_; }
    uint16_t get_remote_port() const { return remote_port_; }
    uint32_t get_send_conn_id() const { return send_conn_id_; }

    void set_data_handler(DataHandler h) { data_handler_ = std::move(h); }
    void set_state_change_handler(StateChangeHandler h) { state_change_handler_ = std::move(h); }

private:
    void send_syn();
    void send_state(uint32_t ack_nr);
    void send_data_packet(uint32_t seq_nr, const uint8_t *data, size_t len);
    void send_fin();
    void send_reset();
    void flush_send_queue();

    void handle_syn(const UtpHeader &hdr, const uint8_t *payload, size_t len);
    void handle_state(const UtpHeader &hdr);
    void handle_data(const UtpHeader &hdr, const uint8_t *payload, size_t len);
    void handle_fin(const UtpHeader &hdr);
    void handle_reset();

    void retransmit_timeout();
    uint32_t current_timestamp_us() const;

    void send_raw(const uint8_t *data, size_t len);

private:
    State state_;

    std::string remote_ip_;
    uint16_t remote_port_;
    std::vector<uint8_t> info_hash_;
    std::string local_peer_id_;

    uint32_t recv_conn_id_;  // conn_id we use when sending
    uint32_t send_conn_id_;  // conn_id we expect to receive

    uint32_t seq_nr_ = 0;
    uint32_t ack_nr_ = 0;

    uint8_t window_size_ = 64;  // in units of 1024 bytes

    // Retransmission state
    struct SentPacket
    {
        uint32_t seq_nr;
        std::vector<uint8_t> data;
        std::chrono::steady_clock::time_point sent_time;
        uint32_t timestamp;
        bool acked = false;
    };
    std::unordered_map<uint32_t, SentPacket> sent_packets_;
    std::queue<SentPacket> send_queue_;  // packets waiting to be sent

    // Congestion control (simplified LEDBAT)
    uint32_t rtt_us_ = 500000;        // estimated RTT (500ms default)
    uint32_t rtt_var_us_ = 200000;    // RTT variance
    uint32_t cwnd_ = 1024;            // congestion window (bytes)
    uint32_t bytes_in_flight_ = 0;

    timer::Timer *retransmit_timer_ = nullptr;
    timer::TimerManager *timer_manager_;
    net::UdpAcceptor *acceptor_;
    net::EventLoop *ev_loop_;

    DataHandler data_handler_;
    StateChangeHandler state_change_handler_;
};

// uTP connection manager that multiplexes multiple uTP connections
// over a single UDP socket. Handles conn_id assignment and dispatches
// incoming packets to the correct UtpConnection.
class UtpManager : public net::ConnectionHandler
{
public:
    using NewUtpPeerCallback = std::function<void(UtpConnection *)>;

    UtpManager();
    ~UtpManager();

    bool start(const NatConfig &config,
               net::EventLoop *loop,
               timer::TimerManager *timer_mgr);

    void stop();
    bool is_running() const { return running_; }
    int32_t get_port() const { return port_; }

    void set_new_peer_callback(NewUtpPeerCallback cb) { new_peer_cb_ = std::move(cb); }

    // Connect to a peer over uTP (initiates NAT hole punch)
    UtpConnection *connect(const std::string &ip, uint16_t port,
                           const std::vector<uint8_t> &info_hash,
                           const std::string &peer_id);

    // Called when a UDP packet is received on the uTP port
    void on_udp_data(const uint8_t *data, size_t len,
                     const std::string &remote_ip, uint16_t remote_port);

    void remove_connection(uint32_t conn_id);

    // ConnectionHandler interface (for UdpAcceptor)
    void on_connected(net::Connection *conn) override;
    void on_error(net::Connection *conn) override;
    void on_read(net::Connection *conn) override;
    void on_write(net::Connection *conn) override;
    void on_close(net::Connection *conn) override;

private:
    uint32_t allocate_conn_id();
    void handle_new_syn(const UtpHeader &hdr, const std::string &remote_ip,
                        uint16_t remote_port, const uint8_t *payload, size_t len);

private:
    bool running_ = false;
    int32_t port_ = 0;

    net::UdpAcceptor *acceptor_ = nullptr;
    net::EventLoop *ev_loop_ = nullptr;
    timer::TimerManager *timer_manager_ = nullptr;

    std::unordered_map<uint32_t, UtpConnection *> connections_;
    std::unordered_map<std::string, UtpConnection *> pending_syn_;  // "ip:port" -> conn
    uint32_t next_conn_id_ = 1;

    NewUtpPeerCallback new_peer_cb_;
};

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_NAT_UTP_CONNECTION_H__
