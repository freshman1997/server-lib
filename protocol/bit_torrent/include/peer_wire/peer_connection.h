#ifndef __BIT_TORRENT_PEER_WIRE_PEER_CONNECTION_H__
#define __BIT_TORRENT_PEER_WIRE_PEER_CONNECTION_H__

#include "peer_wire_message.h"
#include "state/piece_download_state.h"
#include "torrent_meta.h"
#include "buffer/byte_buffer.h"
#include "net/handler/connection_handler.h"
#include "net/runtime/network_runtime.h"
#include "timer/timer_handle.h"
#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <cstdint>

namespace yuan::net
{
    class Connection;
    class TcpConnector;
    class InetAddress;
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
    using PieceDataHandler = std::function<void(PeerConnection * peer,
                                                uint32_t piece_index,
                                                uint32_t offset,
                                                const uint8_t * data,
                                                uint32_t length)>;
    using PieceRequestHandler = std::function<bool(uint32_t piece_index,
                                                   uint32_t offset,
                                                   uint32_t length,
                                                   std::vector<uint8_t> & out)>;
    using PieceServedHandler = std::function<void(uint32_t piece_index,
                                                  uint32_t offset,
                                                  uint32_t length)>;

    using PeerConnectionHandler = std::function<void(PeerConnection * peer)>;

    using ExtendedMessageHandler = std::function<void(PeerConnection * peer,
                                                      uint8_t ext_id,
                                                      const uint8_t * payload,
                                                      size_t len)>;
    using SuggestPieceHandler = std::function<void(PeerConnection * peer, uint32_t piece_index)>;
    using AllowedFastHandler = std::function<void(PeerConnection * peer, uint32_t piece_index)>;
    using RejectRequestHandler = std::function<void(PeerConnection * peer,
                                                    uint32_t piece_index,
                                                    uint32_t offset,
                                                    uint32_t length)>;
    using DhtPortHandler = std::function<void(PeerConnection * peer, uint16_t port)>;

    using SendHandler = std::function<void(const uint8_t * data, size_t len)>;

    // Represents a single TCP connection to a BitTorrent peer
    // Handles the full Peer Wire Protocol (BEP 3) lifecycle:
    // handshake -> bitfield -> request/piece exchange -> keepalive
    class PeerConnection : public ConnectionHandler
    {
        friend class PeerConnectorHandler;

    public:
        enum class State {
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

        void set_connection(const std::shared_ptr<net::Connection> &conn)
        {
            conn_owner_ = conn;
            conn_ = conn_owner_ ? &*conn_owner_ : nullptr;
        }
        void set_connection(net::Connection *conn)
        {
            conn_owner_.reset(conn, [](net::Connection *) {});
            conn_ = conn;
        }

    public:
        // ConnectionHandler interface
        void on_connected(net::Connection &conn) override;
        void on_error(net::Connection &conn) override;
        void on_read(net::Connection &conn) override;
        void on_write(net::Connection &conn) override;
        void on_close(net::Connection &conn) override;

    public:
        // Initiate outbound connection to a peer
        void connect(const std::string &peer_ip,
                     uint16_t peer_port,
                     const TorrentMeta &meta,
                     const std::string &peer_id,
                     net::NetworkRuntime *runtime);

        void accept_inbound(net::Connection *conn,
                            const std::string &remote_peer_id,
                            const std::vector<uint8_t> &info_hash,
                            const std::string &local_peer_id,
                            const std::string &peer_ip,
                            uint16_t peer_port,
                            int32_t total_pieces,
                            net::NetworkRuntime *runtime);
        void accept_inbound(const std::shared_ptr<net::Connection> &conn,
                            const std::string &remote_peer_id,
                            const std::vector<uint8_t> &info_hash,
                            const std::string &local_peer_id,
                            const std::string &peer_ip,
                            uint16_t peer_port,
                            int32_t total_pieces,
                            net::NetworkRuntime *runtime);

        void setup_utp(const std::vector<uint8_t> &info_hash,
                       const std::string &local_peer_id,
                       const std::string &peer_ip,
                       uint16_t peer_port,
                       int32_t total_pieces,
                       net::NetworkRuntime *runtime);

        void feed_data(const uint8_t *data, size_t len);

        void disconnect();

        void send_keepalive();
        void send_choke();
        void send_unchoke();
        void send_interested();
        void send_not_interested();
        void send_have(uint32_t piece_index);
        void send_bitfield(const std::vector<uint8_t> &bits);
        void send_request(uint32_t piece_index, uint32_t offset, uint32_t length);
        void send_piece(uint32_t piece_index, uint32_t offset, const uint8_t *data, uint32_t length);
        void send_cancel(uint32_t piece_index, uint32_t offset, uint32_t length);

        void send_suggest_piece(uint32_t piece_index);
        void send_have_all();
        void send_have_none();
        void send_reject_request(uint32_t piece_index, uint32_t offset, uint32_t length);
        void send_allowed_fast(uint32_t piece_index);
        void send_extended(uint8_t ext_id, const uint8_t *payload, size_t len);
        void send_port(uint16_t port);

        State get_state() const
        {
            return state_;
        }
        const PeerState &get_peer_state() const
        {
            return peer_state_;
        }
        PeerState &mutable_peer_state()
        {
            return peer_state_;
        }
        const std::string &get_peer_ip() const
        {
            return peer_ip_;
        }
        uint16_t get_peer_port() const
        {
            return peer_port_;
        }
        const std::string &get_peer_id() const
        {
            return remote_peer_id_;
        }

        void set_piece_data_handler(PieceDataHandler handler)
        {
            piece_data_handler_ = std::move(handler);
        }
        void set_piece_request_handler(PieceRequestHandler handler)
        {
            piece_request_handler_ = std::move(handler);
        }
        void set_piece_served_handler(PieceServedHandler handler)
        {
            piece_served_handler_ = std::move(handler);
        }
        void set_on_state_change(PeerConnectionHandler handler)
        {
            on_state_change_ = std::move(handler);
        }
        void set_on_unchoke(PeerConnectionHandler handler)
        {
            on_unchoke_ = std::move(handler);
        }
        void set_piece_availability_handler(PeerConnectionHandler handler)
        {
            piece_availability_handler_ = std::move(handler);
        }
        void set_extended_message_handler(ExtendedMessageHandler handler)
        {
            extended_message_handler_ = std::move(handler);
        }
        void set_suggest_piece_handler(SuggestPieceHandler handler)
        {
            suggest_piece_handler_ = std::move(handler);
        }
        void set_allowed_fast_handler(AllowedFastHandler handler)
        {
            allowed_fast_handler_ = std::move(handler);
        }
        void set_reject_request_handler(RejectRequestHandler handler)
        {
            reject_request_handler_ = std::move(handler);
        }
        void set_dht_port_handler(DhtPortHandler handler)
        {
            dht_port_handler_ = std::move(handler);
        }
        void set_send_handler(SendHandler handler)
        {
            send_handler_ = std::move(handler);
        }

        bool is_connected() const
        {
            return state_ == State::connected;
        }
        bool can_download() const
        {
            return state_ == State::connected && !peer_state_.peer_choking && peer_state_.am_interested;
        }
        uint32_t pending_request_count() const
        {
            return pending_request_count_;
        }
        uint32_t request_window_size() const
        {
            return request_window_size_;
        }
        void set_request_window_size(uint32_t size)
        {
            request_window_size_ = size;
        }
        std::vector<PieceBlockRequest> take_pending_requests();

        double download_rate() const { return download_rate_; }
        double upload_rate() const { return upload_rate_; }
        uint64_t last_piece_time_ms() const { return last_piece_time_ms_; }
        bool is_snubbed() const;
        void record_piece_received(uint32_t length);
        void record_piece_sent(uint32_t length);
        void update_rates(uint64_t now_ms);

    private:
        void schedule_connect_failure();
        void schedule_connect_cleanup();
        void cleanup_connect_attempt();
        void fail_connect_result();
        void fail_connect_timeout();
        void on_keepalive_timer(timer::Timer *timer);
        void handle_handshake(const uint8_t *data, size_t len);
        void handle_message(const uint8_t *data, size_t len);
        void send_raw(const std::vector<uint8_t> &data);

    private:
        State state_;
        std::string peer_ip_;
        uint16_t peer_port_;
        std::string local_peer_id_;
        std::string remote_peer_id_;
        std::vector<uint8_t> info_hash_;

        net::Connection *conn_;
        std::shared_ptr<net::Connection> conn_owner_;
        net::NetworkRuntime *runtime_;
        timer::TimerHandle keepalive_timer_;
        timer::TimerHandle connect_timeout_timer_;
        std::unique_ptr<net::InetAddress> pending_addr_;
        std::unique_ptr<net::TcpConnector> pending_connector_;
        std::shared_ptr<PeerConnectorHandler> connector_handler_;

        PeerState peer_state_;
        yuan::buffer::ByteBuffer inbound_buffer_;

        PieceDataHandler piece_data_handler_;
        PieceRequestHandler piece_request_handler_;
        PieceServedHandler piece_served_handler_;
        PeerConnectionHandler on_state_change_;
        PeerConnectionHandler on_unchoke_;
        PeerConnectionHandler piece_availability_handler_;
        ExtendedMessageHandler extended_message_handler_;
        SuggestPieceHandler suggest_piece_handler_;
        AllowedFastHandler allowed_fast_handler_;
        RejectRequestHandler reject_request_handler_;
        DhtPortHandler dht_port_handler_;
        SendHandler send_handler_;
        std::vector<PieceBlockRequest> pending_requests_;

        int32_t total_pieces_;
        uint32_t default_request_size_;
        uint32_t pending_request_count_ = 0;
        uint32_t request_window_size_ = 64;

        double download_rate_ = 0.0;
        double upload_rate_ = 0.0;
        uint64_t last_piece_time_ms_ = 0;
        uint64_t rate_last_update_ms_ = 0;
        uint64_t rate_downloaded_bytes_ = 0;
        uint64_t rate_uploaded_bytes_ = 0;
        static constexpr uint64_t SNUB_THRESHOLD_MS = 60000;
    };

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_PEER_WIRE_PEER_CONNECTION_H__
