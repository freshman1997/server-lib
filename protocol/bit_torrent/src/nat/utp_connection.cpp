#include "nat/utp_connection.h"
#include "nat/nat_config.h"
#include "net/acceptor/udp_acceptor.h"
#include "net/socket/socket.h"
#include "net/connection/connection.h"
#include "net/socket/inet_address.h"
#include "event/event_loop.h"
#include "timer/timer.h"
#include "buffer/buffer.h"
#include "buffer/pool.h"
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

namespace yuan::net::bit_torrent
{

// ===== UtpConnection =====

UtpConnection::UtpConnection(const std::string &remote_ip, uint16_t remote_port,
                             const std::vector<uint8_t> &info_hash,
                             const std::string &peer_id,
                             uint32_t recv_conn_id,
                             uint32_t send_conn_id,
                             net::UdpAcceptor *acceptor,
                             net::EventLoop *loop,
                             timer::TimerManager *timer_mgr)
    : state_(State::idle),
      remote_ip_(remote_ip),
      remote_port_(remote_port),
      info_hash_(info_hash),
      local_peer_id_(peer_id),
      recv_conn_id_(recv_conn_id),
      send_conn_id_(send_conn_id),
      seq_nr_(0),
      ack_nr_(0),
      window_size_(64),
      rtt_us_(500000),
      rtt_var_us_(200000),
      cwnd_(1024),
      bytes_in_flight_(0),
      retransmit_timer_(nullptr),
      timer_manager_(timer_mgr),
      acceptor_(acceptor),
      ev_loop_(loop)
{
    std::random_device rd;
    seq_nr_ = rd() & 0xFFFFFFFF;
}

UtpConnection::~UtpConnection()
{
    close();
}

void UtpConnection::accept_incoming(uint32_t send_conn_id, uint32_t recv_conn_id)
{
    send_conn_id_ = send_conn_id;
    recv_conn_id_ = recv_conn_id;
    state_ = State::syn_recv;

    // Send ST_STATE back
    send_state(seq_nr_);

    // Start retransmit timer
    if (timer_manager_)
    {
        retransmit_timer_ = timer_manager_->interval(500, 500, this, -1);
    }

    if (state_change_handler_)
        state_change_handler_(this, state_);
}

void UtpConnection::on_timer(timer::Timer *timer)
{
    if (timer != retransmit_timer_) return;
    retransmit_timeout();
}

void UtpConnection::send_data(const uint8_t *data, size_t len)
{
    if (state_ != State::connected) return;

    // Segment data into chunks (1500 byte MTU - 20 byte header = 1480 bytes max)
    constexpr size_t MAX_SEGMENT = 1480;
    size_t offset = 0;

    while (offset < len)
    {
        size_t chunk_size = std::min(len - offset, MAX_SEGMENT);
        SentPacket pkt;
        pkt.seq_nr = seq_nr_++;
        pkt.data.assign(data + offset, data + offset + chunk_size);
        pkt.sent_time = std::chrono::steady_clock::now();
        pkt.timestamp = current_timestamp_us();
        pkt.acked = false;

        send_queue_.push(pkt);
        sent_packets_[pkt.seq_nr] = pkt;
        offset += chunk_size;
    }

    // Try to send queued packets
    flush_send_queue();
}

void UtpConnection::on_packet_received(const uint8_t *data, size_t len)
{
    if (len < 4) return;

    // Check if it's a ST_RESET (8 bytes minimum for other types)
    UtpType type = static_cast<UtpType>(data[0] & 0x0F);
    uint8_t version = (data[0] >> 4) & 0x0F;

    if (type == UtpType::st_reset)
    {
        handle_reset();
        return;
    }

    if (len < UTP_HEADER_SIZE) return;

    UtpHeader hdr;
    std::memcpy(&hdr, data, UTP_HEADER_SIZE);
    hdr.to_host_order();

    switch (type)
    {
    case UtpType::st_syn:
        handle_syn(hdr, data + UTP_HEADER_SIZE, len - UTP_HEADER_SIZE);
        break;
    case UtpType::st_state:
        handle_state(hdr);
        break;
    case UtpType::st_data:
        handle_data(hdr, data + UTP_HEADER_SIZE, len - UTP_HEADER_SIZE);
        break;
    case UtpType::st_fin:
        handle_fin(hdr);
        break;
    default:
        break;
    }
}

void UtpConnection::close()
{
    if (state_ == State::closed || state_ == State::idle) return;

    if (state_ == State::connected || state_ == State::syn_recv || state_ == State::syn_sent)
    {
        send_fin();
    }

    state_ = State::closed;

    if (retransmit_timer_)
    {
        retransmit_timer_->cancel();
        retransmit_timer_ = nullptr;
    }

    sent_packets_.clear();
    send_queue_ = std::queue<SentPacket>();
}

void UtpConnection::send_syn()
{
    UtpHeader hdr;
    std::memset(&hdr, 0, UTP_HEADER_SIZE);
    hdr.set_type_ver(UtpType::st_syn);
    hdr.conn_id = recv_conn_id_;
    hdr.seq_nr = seq_nr_;
    hdr.timestamp = current_timestamp_us();
    hdr.timestamp_diff = 0;
    hdr.to_network_order();

    send_raw(reinterpret_cast<const uint8_t *>(&hdr), UTP_HEADER_SIZE);
    state_ = State::syn_sent;

    // Start retransmit timer
    if (timer_manager_)
    {
        retransmit_timer_ = timer_manager_->interval(500, 500, this, -1);
    }

    if (state_change_handler_)
        state_change_handler_(this, state_);
}

void UtpConnection::send_state(uint32_t ack_nr)
{
    UtpHeader hdr;
    std::memset(&hdr, 0, UTP_HEADER_SIZE);
    hdr.set_type_ver(UtpType::st_state);
    hdr.conn_id = recv_conn_id_;
    hdr.seq_nr = seq_nr_;
    hdr.ack_nr = ack_nr;
    hdr.timestamp = current_timestamp_us();
    hdr.timestamp_diff = 0;
    hdr.window_size = window_size_;
    hdr.to_network_order();

    send_raw(reinterpret_cast<const uint8_t *>(&hdr), UTP_HEADER_SIZE);
}

void UtpConnection::send_data_packet(uint32_t seq_nr, const uint8_t *data, size_t len)
{
    std::vector<uint8_t> packet(UTP_HEADER_SIZE + len);
    UtpHeader hdr;
    std::memset(&hdr, 0, UTP_HEADER_SIZE);
    hdr.set_type_ver(UtpType::st_data);
    hdr.conn_id = recv_conn_id_;
    hdr.seq_nr = seq_nr;
    hdr.ack_nr = ack_nr_;
    hdr.timestamp = current_timestamp_us();
    hdr.timestamp_diff = 0;
    hdr.window_size = window_size_;
    hdr.to_network_order();

    std::memcpy(packet.data(), &hdr, UTP_HEADER_SIZE);
    if (len > 0)
        std::memcpy(packet.data() + UTP_HEADER_SIZE, data, len);

    send_raw(packet.data(), packet.size());
}

void UtpConnection::send_fin()
{
    UtpHeader hdr;
    std::memset(&hdr, 0, UTP_HEADER_SIZE);
    hdr.set_type_ver(UtpType::st_fin);
    hdr.conn_id = recv_conn_id_;
    hdr.seq_nr = seq_nr_;
    hdr.ack_nr = ack_nr_;
    hdr.timestamp = current_timestamp_us();
    hdr.timestamp_diff = 0;
    hdr.window_size = window_size_;
    hdr.to_network_order();

    send_raw(reinterpret_cast<const uint8_t *>(&hdr), UTP_HEADER_SIZE);
}

void UtpConnection::send_reset()
{
    uint8_t hdr[8];
    std::memset(hdr, 0, 8);
    hdr[0] = static_cast<uint8_t>(UtpType::st_reset);
    hdr[0] |= (1 << 4); // version 1
    // conn_id is send_conn_id (the one the other side uses for us)
    hdr[2] = (send_conn_id_ >> 8) & 0xFF;
    hdr[3] = send_conn_id_ & 0xFF;
    hdr[4] = (seq_nr_ >> 24) & 0xFF;
    hdr[5] = (seq_nr_ >> 16) & 0xFF;
    hdr[6] = (seq_nr_ >> 8) & 0xFF;
    hdr[7] = seq_nr_ & 0xFF;

    send_raw(hdr, 8);
}

void UtpConnection::handle_syn(const UtpHeader &hdr, const uint8_t *payload, size_t len)
{
    if (state_ != State::idle) return;

    // Verify connection ID matches our send_conn_id
    if (hdr.conn_id != send_conn_id_) return;

    // The initiator's recv_conn_id is our send_conn_id + 1
    recv_conn_id_ = hdr.conn_id + 1;
    send_conn_id_ = hdr.conn_id;
    ack_nr_ = hdr.seq_nr;

    state_ = State::syn_recv;

    // Reply with ST_STATE
    send_state(hdr.seq_nr);

    // Start retransmit timer
    if (timer_manager_)
    {
        retransmit_timer_ = timer_manager_->interval(500, 500, this, -1);
    }

    if (state_change_handler_)
        state_change_handler_(this, state_);
}

void UtpConnection::handle_state(const UtpHeader &hdr)
{
    if (hdr.conn_id != send_conn_id_) return;

    switch (state_)
    {
    case State::syn_sent:
    {
        // Connection established! The peer acknowledged our SYN.
        ack_nr_ = hdr.seq_nr;
        seq_nr_++; // our SYN is "acked"
        state_ = State::connected;

        // Remove the SYN from sent_packets (it was "acked" by the ST_STATE)
        auto it = sent_packets_.find(seq_nr_ - 1);
        if (it != sent_packets_.end())
            sent_packets_.erase(it);

        if (state_change_handler_)
            state_change_handler_(this, state_);

        // Flush any queued data
        flush_send_queue();
        break;
    }

    case State::syn_recv:
    {
        // The peer acknowledged our ST_STATE, connection is now established
        ack_nr_ = hdr.seq_nr;
        state_ = State::connected;

        if (state_change_handler_)
            state_change_handler_(this, state_);

        // Flush any queued data
        flush_send_queue();
        break;
    }

    case State::connected:
    {
        // ACK for our data packets
        uint32_t new_ack = hdr.seq_nr;
        if (new_ack > ack_nr_)
        {
            // Acknowledge packets up to new_ack
            for (auto it = sent_packets_.begin(); it != sent_packets_.end();)
            {
                if (it->second.seq_nr < new_ack)
                {
                    bytes_in_flight_ -= it->second.data.size();
                    it = sent_packets_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            ack_nr_ = new_ack;
        }
        break;
    }

    default:
        break;
    }

    // Update RTT estimate based on timestamp_diff
    if (hdr.timestamp_diff > 0)
    {
        uint32_t their_delay = hdr.timestamp_diff;
        uint32_t now_us = current_timestamp_us();
        if (now_us > hdr.timestamp)
        {
            uint32_t our_delay = now_us - hdr.timestamp;
            uint32_t rtt = our_delay + their_delay;

            // Smoothed RTT (RFC 6298 simplified)
            if (rtt_us_ == 500000)
            {
                rtt_us_ = rtt;
                rtt_var_us_ = rtt / 2;
            }
            else
            {
                rtt_var_us_ = (3 * rtt_var_us_ + abs(static_cast<int>(rtt_us_ - rtt))) / 4;
                rtt_us_ = (7 * rtt_us_ + rtt) / 8;
            }
        }
    }
}

void UtpConnection::handle_data(const UtpHeader &hdr, const uint8_t *payload, size_t len)
{
    if (hdr.conn_id != send_conn_id_) return;
    if (state_ != State::connected && state_ != State::syn_recv) return;

    // Check if this is the next expected packet (in-order delivery)
    if (hdr.seq_nr == ack_nr_ + 1)
    {
        ack_nr_ = hdr.seq_nr;

        if (len > 0 && data_handler_)
        {
            data_handler_(payload, len);
        }

        // If we were in syn_recv, we're now connected
        if (state_ == State::syn_recv)
        {
            state_ = State::connected;
            if (state_change_handler_)
                state_change_handler_(this, state_);
        }

        // ACK the data
        send_state(ack_nr_);
    }
    else if (hdr.seq_nr <= ack_nr_)
    {
        // Duplicate packet, re-ACK
        send_state(ack_nr_);
    }
    // else: out-of-order, discard (simplified - no reassembly buffer)
}

void UtpConnection::handle_fin(const UtpHeader &hdr)
{
    if (hdr.conn_id != send_conn_id_) return;
    state_ = State::closed;

    if (retransmit_timer_)
    {
        retransmit_timer_->cancel();
        retransmit_timer_ = nullptr;
    }

    if (state_change_handler_)
        state_change_handler_(this, state_);
}

void UtpConnection::handle_reset()
{
    state_ = State::closed;

    if (retransmit_timer_)
    {
        retransmit_timer_->cancel();
        retransmit_timer_ = nullptr;
    }

    if (state_change_handler_)
        state_change_handler_(this, state_);
}

void UtpConnection::retransmit_timeout()
{
    if (state_ != State::syn_sent && state_ != State::connected && state_ != State::syn_recv)
        return;

    auto now = std::chrono::steady_clock::now();

    // Retransmit unacked packets
    for (auto &pair : sent_packets_)
    {
        SentPacket &pkt = pair.second;
        if (pkt.acked) continue;

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            now - pkt.sent_time).count();

        if (elapsed > static_cast<int64_t>(rtt_us_ + rtt_var_us_ * 4))
        {
            // Retransmit
            if (pair.first == seq_nr_ - 1 && state_ == State::syn_sent)
            {
                // Retransmit SYN
                send_syn();
            }
            else
            {
                send_data_packet(pkt.seq_nr, pkt.data.data(), pkt.data.size());
                pkt.sent_time = now;
                pkt.timestamp = current_timestamp_us();
            }

            // Increase timeout (exponential backoff)
            rtt_us_ = std::min(rtt_us_ * 2, static_cast<uint32_t>(30000000)); // cap at 30s
        }
    }
}

uint32_t UtpConnection::current_timestamp_us() const
{
    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    return static_cast<uint32_t>(us & 0xFFFFFFFF);
}

void UtpConnection::send_raw(const uint8_t *data, size_t len)
{
    if (!acceptor_) return;

    auto *buf = buffer::BufferedPool::get_instance()->allocate(len);
    buf->write_string(reinterpret_cast<const char *>(data), len);

    net::InetAddress addr(remote_ip_, remote_port_);
    acceptor_->send_to(addr, buf);
}

void UtpConnection::flush_send_queue()
{
    if (state_ != State::connected) return;

    while (!send_queue_.empty() && bytes_in_flight_ < cwnd_)
    {
        SentPacket &pkt = send_queue_.front();
        send_data_packet(pkt.seq_nr, pkt.data.data(), pkt.data.size());
        pkt.sent_time = std::chrono::steady_clock::now();
        bytes_in_flight_ += pkt.data.size();
        send_queue_.pop();
    }
}

// ===== UtpManager =====

UtpManager::UtpManager()
    : running_(false),
      port_(0),
      acceptor_(nullptr),
      ev_loop_(nullptr),
      timer_manager_(nullptr),
      next_conn_id_(1)
{
}

UtpManager::~UtpManager()
{
    stop();
}

bool UtpManager::start(const NatConfig &config,
                        net::EventLoop *loop,
                        timer::TimerManager *timer_mgr)
{
    if (running_) return true;

    ev_loop_ = loop;
    timer_manager_ = timer_mgr;

    int32_t bind_port = config.utp_port > 0 ? config.utp_port : config.listen_port;

    auto *sock = new net::Socket("", bind_port, true); // UDP socket
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

    // Set connection handler for incoming UDP packets
    acceptor_->set_connection_handler(this);
    acceptor_->set_event_handler(ev_loop_);
    ev_loop_->update_channel(acceptor_->get_channel());

    port_ = bind_port;
    running_ = true;
    return true;
}

void UtpManager::stop()
{
    if (!running_) return;
    running_ = false;

    for (auto &pair : connections_)
    {
        pair.second->close();
        delete pair.second;
    }
    connections_.clear();

    for (auto &pair : pending_syn_)
    {
        pair.second->close();
        delete pair.second;
    }
    pending_syn_.clear();

    if (acceptor_)
    {
        acceptor_->close();
        delete acceptor_;
        acceptor_ = nullptr;
    }
}

UtpConnection *UtpManager::connect(const std::string &ip, uint16_t port,
                                    const std::vector<uint8_t> &info_hash,
                                    const std::string &peer_id)
{
    if (!running_) return nullptr;

    uint32_t conn_id = allocate_conn_id();

    // For outgoing connections:
    //   recv_conn_id: we use this to send to the peer
    //   send_conn_id: we expect this from the peer (= recv_conn_id + 1)
    auto *conn = new UtpConnection(ip, port, info_hash, peer_id,
                                    conn_id, conn_id + 1,
                                    acceptor_, ev_loop_, timer_manager_);

    connections_[conn_id] = conn;
    conn->send_syn();

    return conn;
}

void UtpManager::on_udp_data(const uint8_t *data, size_t len,
                              const std::string &remote_ip, uint16_t remote_port)
{
    if (len < 4) return;

    UtpType type = static_cast<UtpType>(data[0] & 0x0F);

    if (type == UtpType::st_reset)
    {
        // ST_RESET: 8 bytes
        if (len < 8) return;
        uint16_t conn_id = (static_cast<uint16_t>(data[2]) << 8) | data[3];
        auto it = connections_.find(conn_id);
        if (it != connections_.end())
        {
            it->second->on_packet_received(data, len);
            // Connection is now closed by handle_reset
            connections_.erase(it);
        }
        return;
    }

    if (len < UTP_HEADER_SIZE) return;

    UtpHeader hdr;
    std::memcpy(&hdr, data, UTP_HEADER_SIZE);
    hdr.to_host_order();

    uint32_t conn_id = hdr.conn_id;

    // Check if this is for an existing connection
    auto it = connections_.find(conn_id);
    if (it != connections_.end())
    {
        it->second->on_packet_received(data, len);

        // Clean up closed connections
        if (it->second->get_state() == UtpConnection::State::closed)
        {
            delete it->second;
            connections_.erase(it);
        }
        return;
    }

    // Check if this is for a pending SYN (incoming connection)
    std::string key = remote_ip + ":" + std::to_string(remote_port);
    auto pit = pending_syn_.find(key);
    if (pit != pending_syn_.end())
    {
        pit->second->on_packet_received(data, len);

        // If the connection was established, move to connections
        if (pit->second->get_state() == UtpConnection::State::connected ||
            pit->second->get_state() == UtpConnection::State::syn_recv)
        {
            uint32_t id = pit->second->get_send_conn_id();
            connections_[id] = pit->second;
            pending_syn_.erase(pit);

            if (new_peer_cb_)
                new_peer_cb_(pit->second);
        }
        return;
    }

    // New incoming SYN
    if (type == UtpType::st_syn)
    {
        handle_new_syn(hdr, remote_ip, remote_port, data + UTP_HEADER_SIZE, len - UTP_HEADER_SIZE);
    }
}

void UtpManager::handle_new_syn(const UtpHeader &hdr, const std::string &remote_ip,
                                 uint16_t remote_port, const uint8_t *payload, size_t len)
{
    // For incoming connections:
    //   The peer sends conn_id = X
    //   We respond with conn_id = X + 1
    //   We expect conn_id = X from the peer

    uint32_t their_recv_id = hdr.conn_id;
    uint32_t our_recv_id = their_recv_id + 1;

    // Create a new UtpConnection to handle this
    auto *conn = new UtpConnection(remote_ip, remote_port,
                                    std::vector<uint8_t>(), // info_hash set later
                                    "",
                                    our_recv_id,   // recv_conn_id: we use this to send
                                    their_recv_id, // send_conn_id: we expect this from them
                                    acceptor_, ev_loop_, timer_manager_);

    conn->on_packet_received(
        reinterpret_cast<const uint8_t *>(&hdr), UTP_HEADER_SIZE + len);

    std::string key = remote_ip + ":" + std::to_string(remote_port);
    pending_syn_[key] = conn;

    if (conn->get_state() == UtpConnection::State::connected ||
        conn->get_state() == UtpConnection::State::syn_recv)
    {
        uint32_t id = conn->get_send_conn_id();
        connections_[id] = conn;
        pending_syn_.erase(key);

        if (new_peer_cb_)
            new_peer_cb_(conn);
    }
}

uint32_t UtpManager::allocate_conn_id()
{
    return next_conn_id_++;
}

void UtpManager::remove_connection(uint32_t conn_id)
{
    auto it = connections_.find(conn_id);
    if (it != connections_.end())
    {
        it->second->close();
        delete it->second;
        connections_.erase(it);
    }
}

// ConnectionHandler stubs (uTP uses UDP, not TCP connections)
void UtpManager::on_connected(net::Connection *conn) {}
void UtpManager::on_error(net::Connection *conn) {}
void UtpManager::on_read(net::Connection *conn) {}
void UtpManager::on_write(net::Connection *conn) {}
void UtpManager::on_close(net::Connection *conn) {}

} // namespace yuan::net::bit_torrent
