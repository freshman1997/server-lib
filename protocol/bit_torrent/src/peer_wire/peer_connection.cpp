#include "peer_wire/peer_connection.h"
#include "buffer/byte_buffer.h"
#include "net/connector/tcp_connector.h"
#include "net/socket/inet_address.h"
#include "net/connection/connection.h"
#include "net/handler/connector_handler.h"
#include "timer/timer_handle.h"

#include <algorithm>
#include <cstring>
#include <chrono>

namespace yuan::net::bit_torrent
{

    // Connector handler for peer TCP connections
    class PeerConnectorHandler : public net::ConnectorHandler
    {
    public:
        explicit PeerConnectorHandler(PeerConnection *p)
            : parent_(p)
        {
        }

        void on_connect_result(const net::ConnectResult &result) override
        {
            if (result.code != net::ConnectResultCode::success || !result.connection) {
                (void)result.error_code;
                (void)result.attempt_id;
                parent_->state_ = PeerConnection::State::error;
                parent_->schedule_connect_cleanup();
                return;
            }

            const auto &conn = result.connection;
            parent_->set_connection(conn);
            if (conn) {
                conn->set_connection_handler(make_non_owning_handler(parent_));
            }

            parent_->state_ = PeerConnection::State::handshaking;
            HandshakeMessage hs;
            hs.set_info_hash(parent_->info_hash_.data());
            hs.set_peer_id(parent_->local_peer_id_);
            auto hs_data = hs.serialize();
            conn->write(yuan::buffer::ByteBuffer(hs_data.data(), hs_data.size()));

            if (parent_->runtime_) {
                parent_->keepalive_timer_ = parent_->runtime_->schedule_periodic(
                    120000, 120000, [parent = parent_]() { parent->send_keepalive(); }, -1);
            }

            parent_->schedule_connect_cleanup();
        }

    private:
        PeerConnection *parent_;
    };

    PeerConnection::PeerConnection()
        : state_(State::idle),
          peer_port_(0),
          conn_(nullptr),
          runtime_(nullptr),
          inbound_buffer_(HandshakeMessage::HANDSHAKE_SIZE * 2),
          total_pieces_(0),
          default_request_size_(16 * 1024),
          pending_request_count_(0),
          request_window_size_(64)
    {
    }

    PeerConnection::~PeerConnection()
    {
        disconnect();
    }

    void PeerConnection::connect(const std::string & peer_ip,
                                 uint16_t peer_port,
                                 const TorrentMeta & meta,
                                 const std::string & peer_id,
                                 net::NetworkRuntime * runtime)
    {
        peer_ip_ = peer_ip;
        peer_port_ = peer_port;
        local_peer_id_ = peer_id;
        info_hash_ = meta.info_hash_;
        total_pieces_ = meta.info.piece_count();
        runtime_ = runtime;

        state_ = State::connecting;
        inbound_buffer_.clear();
        pending_request_count_ = 0;
        pending_requests_.clear();
        pending_addr_ = std::make_unique<net::InetAddress>(peer_ip, peer_port);
        pending_connector_ = std::make_unique<net::TcpConnector>();
        connector_handler_ = std::make_shared<PeerConnectorHandler>(this);

        runtime->register_connector(pending_connector_, connector_handler_);
        pending_connector_->connect(*pending_addr_);
    }

    void PeerConnection::accept_inbound(net::Connection * conn,
                                        const std::string & remote_peer_id,
                                        const std::vector<uint8_t> & info_hash,
                                        const std::string & local_peer_id,
                                        const std::string & peer_ip,
                                        uint16_t peer_port,
                                        int32_t total_pieces,
                                        net::NetworkRuntime * runtime)
    {
        set_connection(conn);
        conn->set_connection_handler(make_non_owning_handler(this));

        remote_peer_id_ = remote_peer_id;
        info_hash_ = info_hash;
        local_peer_id_ = local_peer_id;
        peer_ip_ = peer_ip;
        peer_port_ = peer_port;
        total_pieces_ = total_pieces;
        runtime_ = runtime;

        state_ = State::connected;
        inbound_buffer_.clear();
        pending_request_count_ = 0;
        pending_requests_.clear();

        if (runtime_) {
            keepalive_timer_ = runtime_->schedule_periodic(
                120000, 120000, [this]() { send_keepalive(); }, -1);
        }

        if (on_state_change_)
            on_state_change_(this);
    }

    void PeerConnection::accept_inbound(const std::shared_ptr<net::Connection> &conn,
                                        const std::string &remote_peer_id,
                                        const std::vector<uint8_t> &info_hash,
                                        const std::string &local_peer_id,
                                        const std::string &peer_ip,
                                        uint16_t peer_port,
                                        int32_t total_pieces,
                                        net::NetworkRuntime *runtime)
    {
        set_connection(conn);
        if (conn) {
            conn->set_connection_handler(make_non_owning_handler(this));
        }

        remote_peer_id_ = remote_peer_id;
        info_hash_ = info_hash;
        local_peer_id_ = local_peer_id;
        peer_ip_ = peer_ip;
        peer_port_ = peer_port;
        total_pieces_ = total_pieces;
        runtime_ = runtime;

        state_ = State::connected;
        inbound_buffer_.clear();
        pending_request_count_ = 0;
        pending_requests_.clear();

        if (runtime_) {
            keepalive_timer_ = runtime_->schedule_periodic(
                120000, 120000, [this]() { send_keepalive(); }, -1);
        }

        if (on_state_change_)
            on_state_change_(this);
    }

    void PeerConnection::setup_utp(const std::vector<uint8_t> & info_hash,
                                   const std::string & local_peer_id,
                                   const std::string & peer_ip,
                                   uint16_t peer_port,
                                   int32_t total_pieces,
                                   net::NetworkRuntime * runtime)
    {
        info_hash_ = info_hash;
        local_peer_id_ = local_peer_id;
        peer_ip_ = peer_ip;
        peer_port_ = peer_port;
        total_pieces_ = total_pieces;
        runtime_ = runtime;
        conn_ = nullptr;

        state_ = State::handshaking;
        inbound_buffer_.clear();
        pending_request_count_ = 0;
        pending_requests_.clear();

        if (runtime_) {
            keepalive_timer_ = runtime_->schedule_periodic(
                120000, 120000, [this]() { send_keepalive(); }, -1);
        }
    }

    void PeerConnection::feed_data(const uint8_t * data, size_t len)
    {
        if (!data || len == 0)
            return;

        inbound_buffer_.append(reinterpret_cast<const char *>(data), len);

        if (state_ == State::handshaking) {
            if (inbound_buffer_.readable_bytes() < HandshakeMessage::HANDSHAKE_SIZE)
                return;

            handle_handshake(reinterpret_cast<const uint8_t *>(inbound_buffer_.read_ptr()),
                             HandshakeMessage::HANDSHAKE_SIZE);
            inbound_buffer_.consume(HandshakeMessage::HANDSHAKE_SIZE);
            inbound_buffer_.compact();

            if (state_ != State::connected)
                return;
        }

        if (state_ == State::connected) {
            handle_message(reinterpret_cast<const uint8_t *>(inbound_buffer_.read_ptr()),
                           inbound_buffer_.readable_bytes());
            inbound_buffer_.compact();
        }
    }

    void PeerConnection::disconnect()
    {
        if (state_ == State::closed || state_ == State::idle)
            return;

        state_ = State::closed;

        if (keepalive_timer_) {
            keepalive_timer_.cancel();
            keepalive_timer_.reset();
        }

        if (conn_) {
            conn_->set_connection_handler(std::shared_ptr<net::ConnectionHandler>{});
            conn_->close();
            conn_ = nullptr;
        }

        cleanup_connect_attempt();

        if (on_state_change_) {
            auto cb = std::move(on_state_change_);
            on_state_change_ = nullptr;
            cb(this);
        }
    }

    void PeerConnection::on_connected(const std::shared_ptr<net::Connection> &conn)
    {
        (void)conn;
    }

    void PeerConnection::on_error(const std::shared_ptr<net::Connection> &conn)
    {
        (void)conn;
        state_ = State::error;
        pending_request_count_ = 0;
        if (on_state_change_)
            on_state_change_(this);
    }

    void PeerConnection::on_close(const std::shared_ptr<net::Connection> &conn)
    {
        (void)conn;
        state_ = State::closed;
        pending_request_count_ = 0;
        if (on_state_change_)
            on_state_change_(this);
    }

    void PeerConnection::on_write(const std::shared_ptr<net::Connection> &conn)
    {
        (void)conn;
    }

    void PeerConnection::on_keepalive_timer(timer::Timer * timer)
    {
        (void)timer;
        send_keepalive();
    }

    void PeerConnection::on_read(const std::shared_ptr<net::Connection> &conn)
    {
        if (!conn) {
            return;
        }
        auto byte_buffer = conn->take_input_byte_buffer();
        if (byte_buffer.readable_bytes() == 0)
            return;

        const auto span = byte_buffer.readable_span();
        inbound_buffer_.append(span.data(), span.size());

        if (state_ == State::handshaking) {
            if (inbound_buffer_.readable_bytes() < HandshakeMessage::HANDSHAKE_SIZE) {
                return;
            }

            handle_handshake(reinterpret_cast<const uint8_t *>(inbound_buffer_.read_ptr()), HandshakeMessage::HANDSHAKE_SIZE);
            inbound_buffer_.consume(HandshakeMessage::HANDSHAKE_SIZE);
            inbound_buffer_.compact();

            if (state_ != State::connected) {
                return;
            }
        }

        if (state_ == State::connected) {
            handle_message(reinterpret_cast<const uint8_t *>(inbound_buffer_.read_ptr()), inbound_buffer_.readable_bytes());
            inbound_buffer_.compact();
        }
    }

    void PeerConnection::cleanup_connect_attempt()
    {
        pending_addr_.reset();
        pending_connector_.reset();
        connector_handler_.reset();
    }

    void PeerConnection::schedule_connect_cleanup()
    {
        if (runtime_) {
            auto *self = this;
            runtime_->dispatch([self]() {
                self->cleanup_connect_attempt();
            });
        } else {
            cleanup_connect_attempt();
        }
    }

    void PeerConnection::handle_handshake(const uint8_t * data, size_t len)
    {
        HandshakeMessage hs;
        if (!hs.deserialize(data, len)) {
            state_ = State::error;
            return;
        }

        if (std::memcmp(hs.info_hash_, info_hash_.data(), 20) != 0) {
            state_ = State::error;
            return;
        }

        remote_peer_id_.assign(reinterpret_cast<const char *>(hs.peer_id_), 20);

        peer_state_.supports_fast = hs.supports_fast();
        peer_state_.supports_extensions = hs.supports_extension();
        peer_state_.supports_dht = hs.supports_dht();

        state_ = State::connected;

        if (on_state_change_)
            on_state_change_(this);
    }

    void PeerConnection::handle_message(const uint8_t * data, size_t len)
    {
        const uint8_t *ptr = data;
        size_t remaining = len;

        while (remaining > 0) {
            if (remaining < 4)
                break;

            uint32_t msg_len = (static_cast<uint32_t>(ptr[0]) << 24) |
                               (static_cast<uint32_t>(ptr[1]) << 16) |
                               (static_cast<uint32_t>(ptr[2]) << 8) |
                               static_cast<uint32_t>(ptr[3]);

            if (msg_len == 0) {
                ptr += 4;
                remaining -= 4;
                continue;
            }

            if (remaining < 4 + msg_len)
                break;

            PeerMessage msg;
            int consumed = PeerMessage::parse(ptr, remaining, msg);
            if (consumed <= 0)
                break;

            switch (msg.id_) {
            case PeerMessageId::choke:
                peer_state_.peer_choking = true;
                break;
            case PeerMessageId::unchoke:
                peer_state_.peer_choking = false;
                if (on_unchoke_) {
                    on_unchoke_(this);
                }
                break;
            case PeerMessageId::interested:
                peer_state_.peer_interested = true;
                break;
            case PeerMessageId::not_interested:
                peer_state_.peer_interested = false;
                break;
            case PeerMessageId::have: {
                uint32_t piece = msg.have_piece_index();
                peer_state_.set_have_piece(piece, total_pieces_);
                if (!peer_state_.peer_choking && on_unchoke_) {
                    on_unchoke_(this);
                }
                break;
            }
            case PeerMessageId::bitfield:
                peer_state_.set_bitfield(msg.payload_, total_pieces_);
                if (!peer_state_.peer_choking && on_unchoke_) {
                    on_unchoke_(this);
                }
                break;
            case PeerMessageId::piece:
            {
                const auto before = pending_requests_.size();
                pending_requests_.erase(
                    std::remove_if(
                        pending_requests_.begin(),
                        pending_requests_.end(),
                        [&msg](const PieceBlockRequest &request) {
                        return request.piece_index_ == msg.piece_block_index() &&
                               request.offset_ == msg.piece_block_offset();
                        }),
                    pending_requests_.end());
                if (pending_requests_.size() < before && pending_request_count_ > 0) {
                    --pending_request_count_;
                }
                record_piece_received(msg.piece_block_size());
                if (piece_data_handler_) {
                    piece_data_handler_(
                        this,
                        msg.piece_block_index(),
                        msg.piece_block_offset(),
                        msg.piece_block_data(),
                        msg.piece_block_size());
                }
                break;
            }
            case PeerMessageId::request:
                if (piece_request_handler_) {
                    std::vector<uint8_t> block;
                    const auto piece = msg.request_piece_index();
                    const auto offset = msg.request_offset();
                    const auto length = msg.request_length();
                    if (piece_request_handler_(piece, offset, length, block) && !block.empty()) {
                        send_piece(piece, offset, block.data(), static_cast<uint32_t>(block.size()));
                        record_piece_sent(static_cast<uint32_t>(block.size()));
                        if (piece_served_handler_) {
                            piece_served_handler_(piece, offset, static_cast<uint32_t>(block.size()));
                        }
                    } else if (peer_state_.supports_fast) {
                        send_reject_request(piece, offset, length);
                    }
                }
                break;
            case PeerMessageId::cancel: {
                auto it = std::find_if(pending_requests_.begin(), pending_requests_.end(),
                                       [&msg](const PieceBlockRequest &r) {
                        return r.piece_index_ == msg.request_piece_index() &&
                               r.offset_ == msg.request_offset() &&
                               r.length_ == msg.request_length();
                });
                if (it != pending_requests_.end()) {
                    pending_requests_.erase(it);
                    if (pending_request_count_ > 0)
                        --pending_request_count_;
                }
                if (peer_state_.supports_fast) {
                    send_reject_request(msg.request_piece_index(), msg.request_offset(), msg.request_length());
                }
                break;
            }
            case PeerMessageId::port:
                if (dht_port_handler_) {
                    dht_port_handler_(this, msg.port_value());
                }
                break;
            case PeerMessageId::suggest_piece:
                if (suggest_piece_handler_) {
                    suggest_piece_handler_(this, msg.suggest_piece_index());
                }
                break;
            case PeerMessageId::have_all:
                peer_state_.set_have_all(total_pieces_);
                if (!peer_state_.peer_choking && on_unchoke_) {
                    on_unchoke_(this);
                }
                break;
            case PeerMessageId::have_none:
                peer_state_.set_have_none(total_pieces_);
                break;
            case PeerMessageId::reject_request:
                pending_requests_.erase(
                    std::remove_if(
                        pending_requests_.begin(),
                        pending_requests_.end(),
                        [&msg](const PieceBlockRequest &r) {
                        return r.piece_index_ == msg.reject_piece_index() &&
                               r.offset_ == msg.reject_offset() &&
                               r.length_ == msg.reject_length();
                        }),
                    pending_requests_.end());
                if (pending_request_count_ > 0) {
                    --pending_request_count_;
                }
                if (reject_request_handler_) {
                    reject_request_handler_(this, msg.reject_piece_index(),
                                            msg.reject_offset(), msg.reject_length());
                }
                break;
            case PeerMessageId::allowed_fast:
                if (allowed_fast_handler_) {
                    allowed_fast_handler_(this, msg.allowed_fast_index());
                }
                break;
            case PeerMessageId::extended:
                if (extended_message_handler_) {
                    extended_message_handler_(this, msg.extended_id(),
                                              msg.extended_payload(),
                                              msg.extended_payload_size());
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

    void PeerConnection::send_raw(const std::vector<uint8_t> & data)
    {
        if (send_handler_) {
            send_handler_(data.data(), data.size());
        } else if (conn_) {
            conn_->write(yuan::buffer::ByteBuffer(data.data(), data.size()));
        }
    }

    void PeerConnection::send_keepalive()
    {
        if (state_ != State::connected)
            return;
        if (!conn_ && !send_handler_)
            return;
        auto msg = PeerMessage::keepalive().serialize();
        send_raw(msg);
    }

    void PeerConnection::send_choke()
    {
        if (state_ != State::connected)
            return;
        if (!conn_ && !send_handler_)
            return;
        peer_state_.am_choking = true;
        auto msg = PeerMessage::choke().serialize();
        send_raw(msg);
    }

    void PeerConnection::send_unchoke()
    {
        if (state_ != State::connected)
            return;
        if (!conn_ && !send_handler_)
            return;
        peer_state_.am_choking = false;
        auto msg = PeerMessage::unchoke().serialize();
        send_raw(msg);
    }

    void PeerConnection::send_interested()
    {
        if (state_ != State::connected)
            return;
        if (!conn_ && !send_handler_)
            return;
        peer_state_.am_interested = true;
        auto msg = PeerMessage::interested().serialize();
        send_raw(msg);
    }

    void PeerConnection::send_not_interested()
    {
        if (state_ != State::connected)
            return;
        if (!conn_ && !send_handler_)
            return;
        peer_state_.am_interested = false;
        auto msg = PeerMessage::not_interested().serialize();
        send_raw(msg);
    }

    void PeerConnection::send_have(uint32_t piece_index)
    {
        if (state_ != State::connected)
            return;
        if (!conn_ && !send_handler_)
            return;
        auto msg = PeerMessage::have(piece_index).serialize();
        send_raw(msg);
    }

    void PeerConnection::send_bitfield(const std::vector<uint8_t> & bits)
    {
        if (state_ != State::connected)
            return;
        if (!conn_ && !send_handler_)
            return;
        auto msg = PeerMessage::bitfield(bits).serialize();
        send_raw(msg);
    }

    void PeerConnection::send_request(uint32_t piece_index, uint32_t offset, uint32_t length)
    {
        if (state_ != State::connected)
            return;
        if (!conn_ && !send_handler_)
            return;
        auto msg = PeerMessage::request(piece_index, offset, length).serialize();
        send_raw(msg);
        pending_requests_.push_back(PieceBlockRequest{ piece_index, offset, length, 0 });
        ++pending_request_count_;
    }

    void PeerConnection::send_piece(uint32_t piece_index, uint32_t offset, const uint8_t * data, uint32_t length)
    {
        if (state_ != State::connected || !data || length == 0)
            return;
        if (!conn_ && !send_handler_)
            return;
        auto msg = PeerMessage::piece(piece_index, offset, data, length).serialize();
        send_raw(msg);
    }

    void PeerConnection::send_cancel(uint32_t piece_index, uint32_t offset, uint32_t length)
    {
        if (state_ != State::connected)
            return;
        if (!conn_ && !send_handler_)
            return;
        auto msg = PeerMessage::cancel(piece_index, offset, length).serialize();
        send_raw(msg);
    }

    void PeerConnection::send_suggest_piece(uint32_t piece_index)
    {
        if (state_ != State::connected || !peer_state_.supports_fast)
            return;
        if (!conn_ && !send_handler_)
            return;
        auto msg = PeerMessage::suggest_piece(piece_index).serialize();
        send_raw(msg);
    }

    void PeerConnection::send_have_all()
    {
        if (state_ != State::connected || !peer_state_.supports_fast)
            return;
        if (!conn_ && !send_handler_)
            return;
        auto msg = PeerMessage::have_all().serialize();
        send_raw(msg);
    }

    void PeerConnection::send_have_none()
    {
        if (state_ != State::connected || !peer_state_.supports_fast)
            return;
        if (!conn_ && !send_handler_)
            return;
        auto msg = PeerMessage::have_none().serialize();
        send_raw(msg);
    }

    void PeerConnection::send_reject_request(uint32_t piece_index, uint32_t offset, uint32_t length)
    {
        if (state_ != State::connected || !peer_state_.supports_fast)
            return;
        if (!conn_ && !send_handler_)
            return;
        auto msg = PeerMessage::reject_request(piece_index, offset, length).serialize();
        send_raw(msg);
    }

    void PeerConnection::send_allowed_fast(uint32_t piece_index)
    {
        if (state_ != State::connected || !peer_state_.supports_fast)
            return;
        if (!conn_ && !send_handler_)
            return;
        auto msg = PeerMessage::allowed_fast(piece_index).serialize();
        send_raw(msg);
    }

    void PeerConnection::send_extended(uint8_t ext_id, const uint8_t * payload, size_t len)
    {
        if (state_ != State::connected || !peer_state_.supports_extensions)
            return;
        if (!conn_ && !send_handler_)
            return;
        auto msg = PeerMessage::extended(ext_id, payload, len).serialize();
        send_raw(msg);
    }

    void PeerConnection::send_port(uint16_t port)
    {
        if (state_ != State::connected)
            return;
        if (!conn_ && !send_handler_)
            return;
        auto msg = PeerMessage::port(port).serialize();
        send_raw(msg);
    }

    std::vector<PieceBlockRequest> PeerConnection::take_pending_requests()
    {
        pending_request_count_ = 0;
        auto pending = std::move(pending_requests_);
        pending_requests_.clear();
        return pending;
    }

    bool PeerConnection::is_snubbed() const
    {
        if (peer_state_.peer_choking || !peer_state_.am_interested)
            return false;
        if (last_piece_time_ms_ == 0)
            return false;
        auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        return (now - last_piece_time_ms_) > SNUB_THRESHOLD_MS;
    }

    void PeerConnection::record_piece_received(uint32_t length)
    {
        rate_downloaded_bytes_ += length;
        last_piece_time_ms_ = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    void PeerConnection::record_piece_sent(uint32_t length)
    {
        rate_uploaded_bytes_ += length;
    }

    void PeerConnection::update_rates(uint64_t now_ms)
    {
        if (rate_last_update_ms_ == 0) {
            rate_last_update_ms_ = now_ms;
            return;
        }
        uint64_t elapsed = now_ms > rate_last_update_ms_ ? (now_ms - rate_last_update_ms_) : 0;
        if (elapsed < 1000)
            return;
        double elapsed_sec = static_cast<double>(elapsed) / 1000.0;
        download_rate_ = static_cast<double>(rate_downloaded_bytes_) / elapsed_sec;
        upload_rate_ = static_cast<double>(rate_uploaded_bytes_) / elapsed_sec;
        rate_downloaded_bytes_ = 0;
        rate_uploaded_bytes_ = 0;
        rate_last_update_ms_ = now_ms;
    }

} // namespace yuan::net::bit_torrent
