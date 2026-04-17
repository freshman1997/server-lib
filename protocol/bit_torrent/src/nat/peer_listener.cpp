#include "nat/peer_listener.h"
#include "nat/nat_config.h"
#include "buffer/byte_buffer.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/acceptor/stream_acceptor.h"
#include "net/acceptor/stream_listener.h"
#include "net/socket/socket.h"
#include "net/connection/connection.h"
#include "net/socket/inet_address.h"
#include "net/runtime/network_runtime.h"
#include "torrent_meta.h"
#include <cstring>
#include <algorithm>

namespace yuan::net::bit_torrent
{

    PeerListener::PeerListener()
        : listening_(false),
          actual_port_(0),
          acceptor_(nullptr),
          listen_socket_(nullptr),
          runtime_(nullptr),
          pieces_have_(nullptr)
    {
    }

    PeerListener::~PeerListener()
    {
        stop();
    }

    bool PeerListener::start(const NatConfig & config,
                             const TorrentMeta & meta,
                             const std::string & peer_id,
                             const std::vector<bool> & pieces_have,
                             net::NetworkRuntime * runtime)
    {
        if (listening_)
            return true;

        runtime_ = runtime;
        info_hash_ = meta.info_hash_;
        local_peer_id_ = peer_id;
        pieces_have_ = &pieces_have;

        int32_t base_port = config.listen_port;
        int32_t range = config.listen_retry_range;

        for (int32_t i = 0; i <= range; i++) {
            if (try_bind_and_listen(base_port + i)) {
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
        if (!sock->valid()) {
            delete sock;
            return false;
        }

        sock->set_reuse(true);
        sock->set_none_block(true);

        if (!sock->bind()) {
            delete sock;
            return false;
        }

        net::StreamAcceptor *acceptor = net::create_stream_acceptor(sock);
        if (!acceptor->listen()) {
            delete acceptor;
            return false;
        }

        runtime_->register_acceptor(acceptor, this, acceptor->listener_channel());

        acceptor_ = acceptor;
        listen_socket_ = sock;
        return true;
    }

    void PeerListener::stop()
    {
        if (!listening_)
            return;
        listening_ = false;

        for (auto &p : pending_) {
            if (p.conn)
                p.conn->close();
            if (p.peer)
                delete p.peer;
        }
        pending_.clear();

        if (acceptor_) {
            acceptor_->close();
            acceptor_ = nullptr;
        }

        listen_socket_ = nullptr;
        runtime_ = nullptr;
        pieces_have_ = nullptr;
    }

    void PeerListener::on_connected(net::Connection * conn)
    {
        auto *peer = new PeerConnection();

        PendingInbound pending;
        pending.conn = conn;
        pending.peer = peer;
        pending_.push_back(pending);
    }

    void PeerListener::on_error(net::Connection * conn)
    {
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (it->conn == conn) {
                delete it->peer;
                pending_.erase(it);
                break;
            }
        }
        conn->close();
    }

    void PeerListener::on_read(net::Connection * conn)
    {
        auto byte_buffer = conn->take_input_byte_buffer();
        if (byte_buffer.readable_bytes() == 0)
            return;

        PendingInbound *entry = nullptr;
        for (auto &p : pending_) {
            if (p.conn == conn) {
                entry = &p;
                break;
            }
        }
        if (!entry)
            return;

        const auto span = byte_buffer.readable_span();
        entry->inbound_buffer.append(span.data(), span.size());

        if (entry->inbound_buffer.readable_bytes() >= HandshakeMessage::HANDSHAKE_SIZE) {
            handle_inbound_handshake(conn, entry->peer);
        }
    }

    void PeerListener::on_write(net::Connection * conn)
    {
    }

    void PeerListener::on_close(net::Connection * conn)
    {
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (it->conn == conn) {
                delete it->peer;
                pending_.erase(it);
                break;
            }
        }
    }

    void PeerListener::handle_inbound_handshake(net::Connection * conn, PeerConnection * peer)
    {
        // Retrieve and remove the pending entry
        uint8_t hs_data[68] = {};
        bool found = false;
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (it->conn == conn && it->peer == peer) {
                std::memcpy(hs_data, it->inbound_buffer.read_ptr(), HandshakeMessage::HANDSHAKE_SIZE);
                it->peer = nullptr; // prevent double-delete
                pending_.erase(it);
                found = true;
                break;
            }
        }
        if (!found) {
            delete peer;
            return;
        }

        // Parse the remote handshake
        HandshakeMessage hs;
        if (!hs.deserialize(hs_data, HandshakeMessage::HANDSHAKE_SIZE)) {
            conn->close();
            delete peer;
            return;
        }

        // Verify info_hash matches our torrent
        if (std::memcmp(hs.info_hash_, info_hash_.data(), 20) != 0) {
            conn->close();
            delete peer;
            return;
        }

        // Handshake valid - reply with our handshake
        HandshakeMessage reply;
        reply.set_info_hash(info_hash_.data());
        reply.set_peer_id(local_peer_id_);
        auto reply_data = reply.serialize();
        conn->write(yuan::buffer::ByteBuffer(reply_data.data(), reply_data.size()));

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
                             runtime_);

        // Notify callback with the ready peer
        if (new_peer_cb_) {
            new_peer_cb_(peer);
        }
    }

} // namespace yuan::net::bit_torrent
