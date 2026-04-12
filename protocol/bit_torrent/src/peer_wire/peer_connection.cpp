#include "peer_wire/peer_connection.h"
#include "buffer/byte_buffer.h"
#include "event/event_loop.h"
#include "net/connector/tcp_connector.h"
#include "net/socket/inet_address.h"
#include "net/connection/connection.h"
#include "net/handler/connector_handler.h"
#include "timer/timer.h"
#include "timer/timer_util.hpp"

#include <algorithm>
#include <cstring>

namespace yuan::net::bit_torrent
{

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
        conn->write(yuan::buffer::ByteBuffer(hs_data.data(), hs_data.size()));

        if (parent_->timer_manager_)
        {
            parent_->keepalive_timer_ = timer::TimerUtil::build_period_timer(
                parent_->timer_manager_, 120000, 120000, parent_, &PeerConnection::on_keepalive_timer, -1);
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
      inbound_buffer_(HandshakeMessage::HANDSHAKE_SIZE * 2),
      total_pieces_(0),
      default_request_size_(16 * 1024),
      pending_request_count_(0),
      request_window_size_(4)
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
    inbound_buffer_.clear();
    pending_request_count_ = 0;
    pending_requests_.clear();

    auto *connector = new net::TcpConnector();
    auto *addr = new net::InetAddress(peer_ip, peer_port);
    auto *handler = new PeerConnectorHandler(this, addr, connector);

    connector->set_data(timer_mgr, handler, loop);
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
    inbound_buffer_.clear();
    pending_request_count_ = 0;
    pending_requests_.clear();

    if (timer_manager_)
    {
        keepalive_timer_ = timer::TimerUtil::build_period_timer(
            timer_manager_, 120000, 120000, this, &PeerConnection::on_keepalive_timer, -1);
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
        conn_->set_connection_handler(nullptr);
        conn_->close();
        conn_ = nullptr;
    }

    if (on_state_change_)
    {
        on_state_change_(this);
    }
}

void PeerConnection::on_connected(net::Connection *conn) {}

void PeerConnection::on_error(net::Connection *conn)
{
    state_ = State::error;
    pending_request_count_ = 0;
    if (on_state_change_)
        on_state_change_(this);
}

void PeerConnection::on_close(net::Connection *conn)
{
    state_ = State::closed;
    pending_request_count_ = 0;
    if (on_state_change_)
        on_state_change_(this);
}

void PeerConnection::on_write(net::Connection *conn) {}

void PeerConnection::on_keepalive_timer(timer::Timer *timer)
{
    (void)timer;
    send_keepalive();
}

void PeerConnection::on_read(net::Connection *conn)
{
    auto byte_buffer = conn->take_input_byte_buffer();
    if (byte_buffer.readable_bytes() == 0) return;

    const auto span = byte_buffer.readable_span();
    inbound_buffer_.append(span.data(), span.size());

    if (state_ == State::handshaking)
    {
        if (inbound_buffer_.readable_bytes() < HandshakeMessage::HANDSHAKE_SIZE)
        {
            return;
        }

        handle_handshake(reinterpret_cast<const uint8_t *>(inbound_buffer_.read_ptr()), HandshakeMessage::HANDSHAKE_SIZE);
        inbound_buffer_.consume(HandshakeMessage::HANDSHAKE_SIZE);
        inbound_buffer_.compact();

        if (state_ != State::connected)
        {
            return;
        }
    }

    if (state_ == State::connected)
    {
        handle_message(reinterpret_cast<const uint8_t *>(inbound_buffer_.read_ptr()), inbound_buffer_.readable_bytes());
        inbound_buffer_.compact();
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
            if (on_state_change_)
            {
                on_state_change_(this);
            }
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
            if (on_state_change_)
            {
                on_state_change_(this);
            }
            break;
        }
        case PeerMessageId::bitfield:
            peer_state_.set_bitfield(msg.payload_, total_pieces_);
            if (on_state_change_)
            {
                on_state_change_(this);
            }
            break;
        case PeerMessageId::piece:
            pending_requests_.erase(
                std::remove_if(
                    pending_requests_.begin(),
                    pending_requests_.end(),
                    [&msg](const PieceBlockRequest &request) {
                        return request.piece_index_ == msg.piece_block_index() &&
                               request.offset_ == msg.piece_block_offset();
                    }),
                pending_requests_.end());
            if (pending_request_count_ > 0)
            {
                --pending_request_count_;
            }
            if (piece_data_handler_)
            {
                piece_data_handler_(
                    this,
                    msg.piece_block_index(),
                    msg.piece_block_offset(),
                    msg.piece_block_data(),
                    msg.piece_block_size());
            }
            break;
        case PeerMessageId::request:
            if (piece_request_handler_)
            {
                std::vector<uint8_t> block;
                const auto piece = msg.request_piece_index();
                const auto offset = msg.request_offset();
                const auto length = msg.request_length();
                if (piece_request_handler_(piece, offset, length, block) && !block.empty())
                {
                    send_piece(piece, offset, block.data(), static_cast<uint32_t>(block.size()));
                    if (piece_served_handler_)
                    {
                        piece_served_handler_(piece, offset, static_cast<uint32_t>(block.size()));
                    }
                }
            }
            break;
        default:
            break;
        }

        ptr += consumed;
        remaining -= consumed;
    }

    inbound_buffer_.consume(len - remaining);
}

void PeerConnection::send_keepalive()
{
    if (!conn_ || state_ != State::connected) return;
    auto msg = PeerMessage::keepalive().serialize();
    conn_->write(yuan::buffer::ByteBuffer(msg.data(), msg.size()));
}

void PeerConnection::send_choke()
{
    if (!conn_ || state_ != State::connected) return;
    peer_state_.am_choking = true;
    auto msg = PeerMessage::choke().serialize();
    conn_->write(yuan::buffer::ByteBuffer(msg.data(), msg.size()));
}

void PeerConnection::send_unchoke()
{
    if (!conn_ || state_ != State::connected) return;
    peer_state_.am_choking = false;
    auto msg = PeerMessage::unchoke().serialize();
    conn_->write(yuan::buffer::ByteBuffer(msg.data(), msg.size()));
}

void PeerConnection::send_interested()
{
    if (!conn_ || state_ != State::connected) return;
    peer_state_.am_interested = true;
    auto msg = PeerMessage::interested().serialize();
    conn_->write(yuan::buffer::ByteBuffer(msg.data(), msg.size()));
}

void PeerConnection::send_not_interested()
{
    if (!conn_ || state_ != State::connected) return;
    peer_state_.am_interested = false;
    auto msg = PeerMessage::not_interested().serialize();
    conn_->write(yuan::buffer::ByteBuffer(msg.data(), msg.size()));
}

void PeerConnection::send_have(uint32_t piece_index)
{
    if (!conn_ || state_ != State::connected) return;
    auto msg = PeerMessage::have(piece_index).serialize();
    conn_->write(yuan::buffer::ByteBuffer(msg.data(), msg.size()));
}

void PeerConnection::send_bitfield(const std::vector<uint8_t> &bits)
{
    if (!conn_ || state_ != State::connected) return;
    auto msg = PeerMessage::bitfield(bits).serialize();
    conn_->write(yuan::buffer::ByteBuffer(msg.data(), msg.size()));
}

void PeerConnection::send_request(uint32_t piece_index, uint32_t offset, uint32_t length)
{
    if (!conn_ || state_ != State::connected) return;
    auto msg = PeerMessage::request(piece_index, offset, length).serialize();
    conn_->write(yuan::buffer::ByteBuffer(msg.data(), msg.size()));
    pending_requests_.push_back(PieceBlockRequest{piece_index, offset, length});
    ++pending_request_count_;
}

void PeerConnection::send_piece(uint32_t piece_index, uint32_t offset, const uint8_t *data, uint32_t length)
{
    if (!conn_ || state_ != State::connected || !data || length == 0) return;
    auto msg = PeerMessage::piece(piece_index, offset, data, length).serialize();
    conn_->write(yuan::buffer::ByteBuffer(msg.data(), msg.size()));
}

void PeerConnection::send_cancel(uint32_t piece_index, uint32_t offset, uint32_t length)
{
    if (!conn_ || state_ != State::connected) return;
    auto msg = PeerMessage::cancel(piece_index, offset, length).serialize();
    conn_->write(yuan::buffer::ByteBuffer(msg.data(), msg.size()));
}

std::vector<PieceBlockRequest> PeerConnection::take_pending_requests()
{
    pending_request_count_ = 0;
    auto pending = std::move(pending_requests_);
    pending_requests_.clear();
    return pending;
}

} // namespace yuan::net::bit_torrent
