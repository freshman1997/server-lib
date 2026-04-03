#ifndef __BIT_TORRENT_PEER_WIRE_PEER_CONNECTION_H__
#define __BIT_TORRENT_PEER_WIRE_PEER_CONNECTION_H__

#include "peer_wire_message.h"
#include "torrent_meta.h"
#include "net/handler/connection_handler.h"
#include "timer/timer_manager.h"
#include "timer/timer_task.h"
#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <cstdint>

namespace yuan::net
{
    class Connection;
    class TcpConnector;
    class ConnectorHandler;
    class EventLoop;
    class Poller;
}

namespace yuan::net::bit_torrent
{

class PeerConnection;

// Forward declaration for connector handler used in PeerConnection::connect
class PeerConnectorHandler;

// Callbacks for piece data received from a peer
using PieceDataHandler = std::function<void(uint32_t piece_index,
                                            uint32_t offset,
                                            const uint8_t *data,
                                            uint32_t length)>;

using PeerConnectionHandler = std::function<void(PeerConnection *peer)>;

// Represents a single TCP connection to a BitTorrent peer
// Handles the full Peer Wire Protocol (BEP 3) lifecycle:
// handshake -> bitfield -> request/piece exchange -> keepalive
class PeerConnection : public ConnectionHandler, public timer::TimerTask
{
    friend class PeerConnectorHandler;
public:
    enum class State
    {
        idle,
        connecting,
        handshaking,
        connected,
        closed,
        error
    };

public:
    PeerConnection();
    ~PeerConnection();

public:
    // ConnectionHandler interface
    void on_connected(net::Connection *conn) override;
    void on_error(net::Connection *conn) override;
    void on_read(net::Connection *conn) override;
    void on_write(net::Connection *conn) override;
    void on_close(net::Connection *conn) override;

public:
    // TimerTask interface
    void on_timer(timer::Timer *timer) override;

public:
    // Initiate outbound connection to a peer
    void connect(const std::string &peer_ip,
                 uint16_t peer_port,
                 const TorrentMeta &meta,
                 const std::string &peer_id,
                 timer::TimerManager *timer_mgr,
                 net::EventLoop *loop);

    // Accept an inbound connection that has already completed the BT handshake.
    // The connection must already have had the remote handshake validated
    // and our handshake reply sent. Sets up the PeerConnection to handle
    // subsequent PWP messages on the given connection.
    void accept_inbound(net::Connection *conn,
                        const std::string &remote_peer_id,
                        const std::vector<uint8_t> &info_hash,
                        const std::string &local_peer_id,
                        const std::string &peer_ip,
                        uint16_t peer_port,
                        int32_t total_pieces,
                        timer::TimerManager *timer_mgr,
                        net::EventLoop *loop);

    void disconnect();

    void send_keepalive();
    void send_choke();
    void send_unchoke();
    void send_interested();
    void send_not_interested();
    void send_have(uint32_t piece_index);
    void send_bitfield(const std::vector<uint8_t> &bits);
    void send_request(uint32_t piece_index, uint32_t offset, uint32_t length);
    void send_cancel(uint32_t piece_index, uint32_t offset, uint32_t length);

    // Request the next block we need from this peer (rarest-first heuristic)
    bool request_next_piece(const std::vector<bool> &we_have);

    State get_state() const { return state_; }
    const PeerState &get_peer_state() const { return peer_state_; }
    PeerState &mutable_peer_state() { return peer_state_; }
    const std::string &get_peer_ip() const { return peer_ip_; }
    uint16_t get_peer_port() const { return peer_port_; }
    const std::string &get_peer_id() const { return remote_peer_id_; }

    void set_piece_data_handler(PieceDataHandler handler) { piece_data_handler_ = std::move(handler); }
    void set_on_state_change(PeerConnectionHandler handler) { on_state_change_ = std::move(handler); }

    bool is_connected() const { return state_ == State::connected; }
    bool can_download() const
    {
        return state_ == State::connected && !peer_state_.peer_choking && peer_state_.am_interested;
    }

private:
    void handle_handshake(const uint8_t *data, size_t len);
    void handle_message(const uint8_t *data, size_t len);

private:
    State state_;
    std::string peer_ip_;
    uint16_t peer_port_;
    std::string local_peer_id_;
    std::string remote_peer_id_;
    std::vector<uint8_t> info_hash_;

    net::Connection *conn_;
    timer::TimerManager *timer_manager_;
    net::EventLoop *ev_loop_;
    timer::Timer *keepalive_timer_;

    PeerState peer_state_;
    uint8_t handshake_recv_[68];
    size_t handshake_received_;

    PieceDataHandler piece_data_handler_;
    PeerConnectionHandler on_state_change_;

    int32_t total_pieces_;
    uint32_t default_request_size_;
};

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_PEER_WIRE_PEER_CONNECTION_H__
