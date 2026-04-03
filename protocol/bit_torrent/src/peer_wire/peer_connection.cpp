#include "peer_wire/peer_connection.h"
#include "net/connector/tcp_connector.h"
#include "net/socket/inet_address.h"
#include "net/connection/connection.h"
#include "net/handler/connector_handler.h"
#include "timer/timer.h"
#include "buffer/buffer.h"
#include "buffer/pool.h"
#include <cstring>

namespace yuan::net::bit_torrent
{

// Helper: create a Buffer from raw data for writing to Connection
static buffer::Buffer *make_write_buffer(const uint8_t *data, size_t len)
{
    auto *buf = buffer::BufferedPool::get_instance()->allocate(len);
    buf->write_string(reinterpret_cast<const char *>(data), len);
    return buf;
}

// Connector handler for peer TCP connections
class PeerConnectorHandler : public net::ConnectorHandler
{
public:
    PeerConnectorHandler(PeerConnection *p, net::InetAddress *a, net::TcpConnector *c)
        : parent_(p), addr_(a), connector_(c) {}

    void on_connect_failed(net::Connection *conn) override
    {
        parent_->state_ = PeerConnection::State::error;
        delete addr_;
        delete connector_;
        delete this;
    }

    void on_connect_timeout(net::Connection *conn) override
    {
        parent_->state_ = PeerConnection::State::error;
        delete addr_;
        delete connector_;
        delete this;
    }

    void on_connected_success(net::Connection *conn) override
    {
        parent_->conn_ = conn;
        conn->set_connection_handler(parent_);

        parent_->state_ = PeerConnection::State::handshaking;
        HandshakeMessage hs;
        hs.set_info_hash(parent_->info_hash_.data());
        hs.set_peer_id(parent_->local_peer_id_);
        auto hs_data = hs.serialize();
        auto *buf = make_write_buffer(hs_data.data(), hs_data.size());
        conn->write(buf);

        if (parent_->timer_manager_)
        {
            parent_->keepalive_timer_ = parent_->timer_manager_->interval(
                120000, 120000, parent_, -1);
        }

        delete addr_;
        delete connector_;
        delete this;
    }

private:
    PeerConnection *parent_;
    net::InetAddress *addr_;
    net::TcpConnector *connector_;
};

PeerConnection::PeerConnection()
    : state_(State::idle),
      peer_port_(0),
      conn_(nullptr),
      timer_manager_(nullptr),
      ev_loop_(nullptr),
      keepalive_timer_(nullptr),
      handshake_received_(0),
      total_pieces_(0),
      default_request_size_(16 * 1024)
{
}

PeerConnection::~PeerConnection() { disconnect(); }

void PeerConnection::connect(const std::string &peer_ip,
                             uint16_t peer_port,
                             const TorrentMeta &meta,
                             const std::string &peer_id,
                             timer::TimerManager *timer_mgr,
                             net::EventLoop *loop)
{
    peer_ip_ = peer_ip;
    peer_port_ = peer_port;
    local_peer_id_ = peer_id;
    info_hash_ = meta.info_hash_;
    total_pieces_ = meta.info.piece_count();
    timer_manager_ = timer_mgr;
    ev_loop_ = loop;

    state_ = State::connecting;
    handshake_received_ = 0;

    auto *connector = new net::TcpConnector();
    auto *addr = new net::InetAddress(peer_ip, peer_port);
    auto *handler = new PeerConnectorHandler(this, addr, connector);

    connector->set_data(timer_mgr, handler, nullptr);
    connector->connect(*addr);
}

void PeerConnection::accept_inbound(net::Connection *conn,
                                    const std::string &remote_peer_id,
                                    const std::vector<uint8_t> &info_hash,
                                    const std::string &local_peer_id,
                                    const std::string &peer_ip,
                                    uint16_t peer_port,
                                    int32_t total_pieces,
                                    timer::TimerManager *timer_mgr,
                                    net::EventLoop *loop)
{
    conn_ = conn;
    conn->set_connection_handler(this);

    remote_peer_id_ = remote_peer_id;
    info_hash_ = info_hash;
    local_peer_id_ = local_peer_id;
    peer_ip_ = peer_ip;
    peer_port_ = peer_port;
    total_pieces_ = total_pieces;
    timer_manager_ = timer_mgr;
    ev_loop_ = loop;

    state_ = State::connected;
    handshake_received_ = HandshakeMessage::HANDSHAKE_SIZE; // skip handshake parsing

    if (timer_manager_)
    {
        keepalive_timer_ = timer_manager_->interval(
            120000, 120000, this, -1);
    }

    if (on_state_change_)
        on_state_change_(this);
}

void PeerConnection::disconnect()
{
    if (state_ == State::closed || state_ == State::idle) return;

    state_ = State::closed;

    if (keepalive_timer_)
    {
        keepalive_timer_->cancel();
        keepalive_timer_ = nullptr;
    }

    if (conn_)
    {
        conn_->close();
        conn_ = nullptr;
    }
}

void PeerConnection::on_connected(net::Connection *conn) {}

void PeerConnection::on_error(net::Connection *conn)
{
    state_ = State::error;
}

void PeerConnection::on_close(net::Connection *conn)
{
    state_ = State::closed;
}

void PeerConnection::on_write(net::Connection *conn) {}

void PeerConnection::on_timer(timer::Timer *timer) {}

void PeerConnection::on_read(net::Connection *conn)
{
    auto *buf = conn->get_input_buff();
    if (!buf || buf->readable_bytes() == 0) return;

    const uint8_t *data = reinterpret_cast<const uint8_t *>(buf->peek());
    size_t len = buf->readable_bytes();

    if (state_ == State::handshaking)
    {
        size_t need = HandshakeMessage::HANDSHAKE_SIZE - handshake_received_;
        size_t avail = std::min(len, need);
        std::memcpy(handshake_recv_ + handshake_received_, data, avail);
        handshake_received_ += avail;
        buf->add_read_index(avail);

        if (handshake_received_ >= HandshakeMessage::HANDSHAKE_SIZE)
        {
            handle_handshake(handshake_recv_, HandshakeMessage::HANDSHAKE_SIZE);
        }
        return;
    }

    if (state_ == State::connected)
    {
        handle_message(data, len);
        buf->reset();
    }
}

void PeerConnection::handle_handshake(const uint8_t *data, size_t len)
{
    HandshakeMessage hs;
    if (!hs.deserialize(data, len))
    {
        state_ = State::error;
        return;
    }

    if (std::memcmp(hs.info_hash_, info_hash_.data(), 20) != 0)
    {
        state_ = State::error;
        return;
    }

    remote_peer_id_.assign(reinterpret_cast<const char *>(hs.peer_id_), 20);
    state_ = State::connected;

    if (on_state_change_)
        on_state_change_(this);
}

void PeerConnection::handle_message(const uint8_t *data, size_t len)
{
    const uint8_t *ptr = data;
    size_t remaining = len;

    while (remaining > 0)
    {
        if (remaining < 4) break;

        uint32_t msg_len = (static_cast<uint32_t>(ptr[0]) << 24) |
                           (static_cast<uint32_t>(ptr[1]) << 16) |
                           (static_cast<uint32_t>(ptr[2]) << 8) |
                           static_cast<uint32_t>(ptr[3]);

        if (msg_len == 0)
        {
            ptr += 4;
            remaining -= 4;
            continue;
        }

        if (remaining < 4 + msg_len) break;

        PeerMessage msg;
        int consumed = PeerMessage::parse(ptr, remaining, msg);
        if (consumed <= 0) break;

        switch (msg.id_)
        {
        case PeerMessageId::choke:
            peer_state_.peer_choking = true;
            break;
        case PeerMessageId::unchoke:
            peer_state_.peer_choking = false;
            break;
        case PeerMessageId::interested:
            peer_state_.peer_interested = true;
            break;
        case PeerMessageId::not_interested:
            peer_state_.peer_interested = false;
            break;
        case PeerMessageId::have:
        {
            uint32_t piece = msg.have_piece_index();
            peer_state_.set_have_piece(piece, total_pieces_);
            break;
        }
        case PeerMessageId::bitfield:
            peer_state_.set_bitfield(msg.payload_, total_pieces_);
            break;
        case PeerMessageId::piece:
            if (piece_data_handler_)
            {
                piece_data_handler_(
                    msg.piece_block_index(),
                    msg.piece_block_offset(),
                    msg.piece_block_data(),
                    msg.piece_block_size());
            }
            break;
        default:
            break;
        }

        ptr += consumed;
        remaining -= consumed;
    }
}

void PeerConnection::send_keepalive()
{
    if (!conn_ || state_ != State::connected) return;
    auto msg = PeerMessage::keepalive().serialize();
    auto *buf = make_write_buffer(msg.data(), msg.size());
    conn_->write(buf);
}

void PeerConnection::send_choke()
{
    if (!conn_ || state_ != State::connected) return;
    peer_state_.am_choking = true;
    auto msg = PeerMessage::choke().serialize();
    auto *buf = make_write_buffer(msg.data(), msg.size());
    conn_->write(buf);
}

void PeerConnection::send_unchoke()
{
    if (!conn_ || state_ != State::connected) return;
    peer_state_.am_choking = false;
    auto msg = PeerMessage::unchoke().serialize();
    auto *buf = make_write_buffer(msg.data(), msg.size());
    conn_->write(buf);
}

void PeerConnection::send_interested()
{
    if (!conn_ || state_ != State::connected) return;
    peer_state_.am_interested = true;
    auto msg = PeerMessage::interested().serialize();
    auto *buf = make_write_buffer(msg.data(), msg.size());
    conn_->write(buf);
}

void PeerConnection::send_not_interested()
{
    if (!conn_ || state_ != State::connected) return;
    peer_state_.am_interested = false;
    auto msg = PeerMessage::not_interested().serialize();
    auto *buf = make_write_buffer(msg.data(), msg.size());
    conn_->write(buf);
}

void PeerConnection::send_have(uint32_t piece_index)
{
    if (!conn_ || state_ != State::connected) return;
    auto msg = PeerMessage::have(piece_index).serialize();
    auto *buf = make_write_buffer(msg.data(), msg.size());
    conn_->write(buf);
}

void PeerConnection::send_bitfield(const std::vector<uint8_t> &bits)
{
    if (!conn_ || state_ != State::connected) return;
    auto msg = PeerMessage::bitfield(bits).serialize();
    auto *buf = make_write_buffer(msg.data(), msg.size());
    conn_->write(buf);
}

void PeerConnection::send_request(uint32_t piece_index, uint32_t offset, uint32_t length)
{
    if (!conn_ || state_ != State::connected) return;
    auto msg = PeerMessage::request(piece_index, offset, length).serialize();
    auto *buf = make_write_buffer(msg.data(), msg.size());
    conn_->write(buf);
}

void PeerConnection::send_cancel(uint32_t piece_index, uint32_t offset, uint32_t length)
{
    if (!conn_ || state_ != State::connected) return;
    auto msg = PeerMessage::cancel(piece_index, offset, length).serialize();
    auto *buf = make_write_buffer(msg.data(), msg.size());
    conn_->write(buf);
}

bool PeerConnection::request_next_piece(const std::vector<bool> &we_have)
{
    if (!can_download()) return false;

    for (int32_t i = 0; i < total_pieces_; i++)
    {
        if (!we_have[i] && peer_state_.has_piece(i))
        {
            send_request(i, 0, default_request_size_);
            return true;
        }
    }

    return false;
}

} // namespace yuan::net::bit_torrent
