#include "nat/peer_listener.h"
#include "nat/nat_config.h"
#include "net/acceptor/tcp_acceptor.h"
#include "net/socket/socket.h"
#include "net/connection/connection.h"
#include "net/socket/inet_address.h"
#include "event/event_loop.h"
#include "buffer/buffer.h"
#include "buffer/pool.h"
#include "torrent_meta.h"
#include <cstring>
#include <algorithm>

namespace yuan::net::bit_torrent
{

// Helper: create a Buffer from raw data for writing to Connection
static buffer::Buffer *make_write_buffer(const uint8_t *data, size_t len)
{
    auto *buf = buffer::BufferedPool::get_instance()->allocate(len);
    buf->write_string(reinterpret_cast<const char *>(data), len);
    return buf;
}

PeerListener::PeerListener()
    : listening_(false),
      actual_port_(0),
      acceptor_(nullptr),
      listen_socket_(nullptr),
      ev_loop_(nullptr),
      timer_manager_(nullptr),
      pieces_have_(nullptr)
{
}

PeerListener::~PeerListener()
{
    stop();
}

bool PeerListener::start(const NatConfig &config,
                         const TorrentMeta &meta,
                         const std::string &peer_id,
                         const std::vector<bool> &pieces_have,
                         net::EventLoop *loop,
                         timer::TimerManager *timer_mgr)
{
    if (listening_) return true;

    ev_loop_ = loop;
    timer_manager_ = timer_mgr;
    info_hash_ = meta.info_hash_;
    local_peer_id_ = peer_id;
    pieces_have_ = &pieces_have;

    int32_t base_port = config.listen_port;
    int32_t range = config.listen_retry_range;

    for (int32_t i = 0; i <= range; i++)
    {
        if (try_bind_and_listen(base_port + i))
        {
            actual_port_ = base_port + i;
            listening_ = true;
            return true;
        }
    }

    return false;
}

bool PeerListener::try_bind_and_listen(int32_t port)
{
    auto *sock = new net::Socket("", port);
    if (!sock->valid())
    {
        delete sock;
        return false;
    }

    sock->set_reuse(true);
    sock->set_none_block(true);

    if (!sock->bind())
    {
        delete sock;
        return false;
    }

    auto *acceptor = new net::TcpAcceptor(sock);
    if (!acceptor->listen())
    {
        delete acceptor;
        return false;
    }

    acceptor->set_connection_handler(this);
    acceptor->set_event_handler(ev_loop_);
    ev_loop_->update_channel(acceptor->get_channel());

    acceptor_ = acceptor;
    listen_socket_ = sock;
    return true;
}

void PeerListener::stop()
{
    if (!listening_) return;
    listening_ = false;

    for (auto &p : pending_)
    {
        if (p.conn) p.conn->close();
        if (p.peer) delete p.peer;
    }
    pending_.clear();

    if (acceptor_)
    {
        acceptor_->close();
        delete acceptor_;
        acceptor_ = nullptr;
    }

    listen_socket_ = nullptr;
    ev_loop_ = nullptr;
    timer_manager_ = nullptr;
    pieces_have_ = nullptr;
}

void PeerListener::on_connected(net::Connection *conn)
{
    auto *peer = new PeerConnection();

    PendingInbound pending;
    pending.conn = conn;
    pending.peer = peer;
    pending.handshake_received = 0;
    pending_.push_back(pending);
}

void PeerListener::on_error(net::Connection *conn)
{
    for (auto it = pending_.begin(); it != pending_.end(); ++it)
    {
        if (it->conn == conn)
        {
            delete it->peer;
            pending_.erase(it);
            break;
        }
    }
    conn->close();
}

void PeerListener::on_read(net::Connection *conn)
{
    auto *buf = conn->get_input_buff();
    if (!buf || buf->readable_bytes() == 0) return;

    PendingInbound *entry = nullptr;
    for (auto &p : pending_)
    {
        if (p.conn == conn)
        {
            entry = &p;
            break;
        }
    }
    if (!entry) return;

    const uint8_t *data = reinterpret_cast<const uint8_t *>(buf->peek());
    size_t len = buf->readable_bytes();

    size_t need = HandshakeMessage::HANDSHAKE_SIZE - entry->handshake_received;
    size_t avail = std::min(len, need);
    std::memcpy(entry->handshake_recv + entry->handshake_received, data, avail);
    entry->handshake_received += avail;
    buf->add_read_index(avail);

    if (entry->handshake_received >= HandshakeMessage::HANDSHAKE_SIZE)
    {
        handle_inbound_handshake(conn, entry->peer);
    }
}

void PeerListener::on_write(net::Connection *conn) {}

void PeerListener::on_close(net::Connection *conn)
{
    for (auto it = pending_.begin(); it != pending_.end(); ++it)
    {
        if (it->conn == conn)
        {
            delete it->peer;
            pending_.erase(it);
            break;
        }
    }
}

void PeerListener::handle_inbound_handshake(net::Connection *conn, PeerConnection *peer)
{
    // Retrieve and remove the pending entry
    uint8_t hs_data[68] = {};
    bool found = false;
    for (auto it = pending_.begin(); it != pending_.end(); ++it)
    {
        if (it->conn == conn && it->peer == peer)
        {
            std::memcpy(hs_data, it->handshake_recv, 68);
            it->peer = nullptr; // prevent double-delete
            pending_.erase(it);
            found = true;
            break;
        }
    }
    if (!found)
    {
        delete peer;
        return;
    }

    // Parse the remote handshake
    HandshakeMessage hs;
    if (!hs.deserialize(hs_data, HandshakeMessage::HANDSHAKE_SIZE))
    {
        conn->close();
        delete peer;
        return;
    }

    // Verify info_hash matches our torrent
    if (std::memcmp(hs.info_hash_, info_hash_.data(), 20) != 0)
    {
        conn->close();
        delete peer;
        return;
    }

    // Handshake valid - reply with our handshake
    HandshakeMessage reply;
    reply.set_info_hash(info_hash_.data());
    reply.set_peer_id(local_peer_id_);
    auto reply_data = reply.serialize();
    auto *write_buf = make_write_buffer(reply_data.data(), reply_data.size());
    conn->write(write_buf);

    // Extract remote peer info
    const auto &remote_addr = conn->get_remote_address();
    std::string peer_ip = remote_addr.get_ip();
    uint16_t peer_port = static_cast<uint16_t>(remote_addr.get_port());
    std::string remote_peer_id(reinterpret_cast<const char *>(hs.peer_id_), 20);

    // Get total pieces from torrent meta (passed via pieces_have_ size)
    int32_t total_pieces = static_cast<int32_t>(pieces_have_->size());

    // Initialize the PeerConnection for inbound use
    peer->accept_inbound(conn, remote_peer_id, info_hash_, local_peer_id_,
                         peer_ip, peer_port, total_pieces,
                         timer_manager_, ev_loop_);

    // Notify callback with the ready peer
    if (new_peer_cb_)
    {
        new_peer_cb_(peer);
    }
}

} // namespace yuan::net::bit_torrent
