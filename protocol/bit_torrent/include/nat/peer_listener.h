#ifndef __BIT_TORRENT_NAT_PEER_LISTENER_H__
#define __BIT_TORRENT_NAT_PEER_LISTENER_H__

#include "peer_wire/peer_connection.h"
#include "buffer/byte_buffer.h"
#include "net/handler/connection_handler.h"
#include "net/runtime/network_runtime.h"
#include <functional>
#include <string>
#include <vector>
#include <cstdint>

namespace yuan::net
{
    class StreamAcceptor;
    class Socket;
    class Connection;
}

namespace yuan::net::bit_torrent
{

    class PeerConnection;
    struct TorrentMeta;
    struct NatConfig;

    // Accepts inbound BitTorrent peer connections on a TCP port.
    // Handles the BT handshake (receiving peer's handshake first, then replying).
    // Implements ConnectionHandler to receive events from the stream listener.
    class PeerListener : public net::ConnectionHandler
    {
    public:
        using NewPeerCallback = std::function<void(std::shared_ptr<PeerConnection> peer)>;

        PeerListener();
        ~PeerListener();

        // Start listening on the configured port (tries port+0..port+range)
        bool start(const NatConfig &config,
                   const TorrentMeta &meta,
                   const std::string &peer_id,
                   const std::vector<bool> &pieces_have,
                   net::NetworkRuntime *runtime);

        void stop();
        bool is_listening() const
        {
            return listening_;
        }
        int32_t get_actual_port() const
        {
            return actual_port_;
        }

        void set_new_peer_callback(NewPeerCallback cb)
        {
            new_peer_cb_ = std::move(cb);
        }

        void set_max_pending(int32_t max_pending)
        {
            max_pending_ = max_pending;
        }

        // ConnectionHandler interface
        void on_connected(net::Connection &conn) override;
        void on_error(net::Connection &conn) override;
        void on_read(net::Connection &conn) override;
        void on_write(net::Connection &conn) override;
        void on_close(net::Connection &conn) override;

    private:
        bool try_bind_and_listen(int32_t port);

        void handle_inbound_handshake(net::Connection &conn, std::shared_ptr<PeerConnection> peer);

    private:
        bool listening_ = false;
        int32_t actual_port_ = 0;

        std::unique_ptr<net::StreamAcceptor> acceptor_;

        net::NetworkRuntime *runtime_ = nullptr;

        std::vector<uint8_t> info_hash_;
        std::string local_peer_id_;
        const std::vector<bool> *pieces_have_ = nullptr;

        NewPeerCallback new_peer_cb_;

        int32_t max_pending_ = 50;

        struct PendingInbound
        {
            std::shared_ptr<net::Connection> conn;
            std::shared_ptr<PeerConnection> peer;
            yuan::buffer::ByteBuffer inbound_buffer;
        };
        std::vector<PendingInbound> pending_;
    };

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_NAT_PEER_LISTENER_H__
