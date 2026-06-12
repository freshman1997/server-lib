#include "websocket_connection.h"
#include "base/owner_ptr.h"
#include "base/time.h"
#include "handshake.h"
#include "net/connection/connection.h"
#include "net/runtime/network_runtime.h"
#include "timer/timer_handle.h"
#include "websocket_config.h"
#include "websocket_packet_parser.h"
#include "websocket_protocol.h"

#include <cassert>
#include "logger.h"

namespace yuan::net::websocket
{
    WebSocketConnection::WebSocketConnection(WorkMode mode)
        : mode_(mode), state_(State::connecting_), conn_(nullptr), config_(nullptr)
    {
    }

    WebSocketConnection::~WebSocketConnection()
    {
        if (heartbeat_timer_) {
            heartbeat_timer_.cancel();
            heartbeat_timer_.reset();
        }
        cancel_close_deadline_timer();
    }

    void WebSocketConnection::bind_connection(Connection * conn)
    {
        conn_owner_.reset();
        conn_ = conn;
    }

    void WebSocketConnection::bind_connection(const std::shared_ptr<Connection> &conn)
    {
        conn_owner_ = conn;
        conn_ = yuan::base::owner_ptr(conn);
    }

    void WebSocketConnection::set_config(WebSocketConfigManager * config)
    {
        config_ = config;
        handshaker_.set_config(config);
        if (config_) {
            if (mode_ == WorkMode::client_) {
                use_mask(config_->is_client_use_mask());
            } else {
                use_mask(config_->is_server_use_mask());
            }
            set_outbound_queue_limits(config_->get_outbound_queue_max_messages(),
                                      config_->get_outbound_queue_max_bytes());
            close_handshake_timeout_ms_ = config_->get_close_handshake_timeout();
            max_message_size_ = config_->get_max_message_size();
            max_fragmented_message_size_ = config_->get_max_fragmented_message_size();
        }
    }

    bool WebSocketConnection::send(const ::yuan::buffer::ByteBuffer & buf, PacketType pktType)
    {
        if (state_ != State::connected_) {
            return false;
        }

        assert(conn_);

        if (pktType == PacketType::close_ || pktType == PacketType::ping_ || pktType == PacketType::pong_) {
            std::vector< ::yuan::buffer::ByteBuffer> output;
            if (pack_control_frame(buf, static_cast<uint8_t>(pktType), output)) {
                return enqueue_output(std::move(output)) && flush_output_queue();
            }
            return false;
        }

        if (pkt_parser_.pack(this, buf, (uint8_t)pktType)) {
            std::vector< ::yuan::buffer::ByteBuffer> output;
            output.reserve(output_chunks_.size());
            for (auto &chunk : output_chunks_) {
                output.push_back(std::move(chunk));
            }
            output_chunks_.clear();
            return enqueue_output(std::move(output)) && flush_output_queue();
        } else {
            LOG_ERROR("cant pack ws frame!");
            conn_->close();
            return false;
        }
    }

    bool WebSocketConnection::send(const char * data, size_t len, PacketType pktType)
    {
        if (state_ != State::connected_) {
            return false;
        }
        return send(::yuan::buffer::ByteBuffer(data, len), pktType);
    }

    void WebSocketConnection::close(WebSocketCloseCode code)
    {
        if (state_ == State::closed_ || close_sent_) {
            return;
        }
        state_ = State::closing_;
        if (conn_) {
            send_close_frame_to(conn_, (uint16_t)code);
        }
    }

    void WebSocketConnection::shutdown()
    {
        if (heartbeat_timer_) {
            heartbeat_timer_.cancel();
            heartbeat_timer_.reset();
        }
        cancel_close_deadline_timer();
        output_chunks_.clear();
        input_chunks_.clear();
        outbound_queue_.clear();
        outbound_queue_bytes_ = 0;
        state_ = State::closed_;
        if (conn_) {
            conn_->close();
        }
    }

    std::shared_ptr<Connection> WebSocketConnection::get_native_connection()
    {
        return conn_owner_.lock();
    }

    const std::string &WebSocketConnection::get_url() const
    {
        return url_;
    }

    void WebSocketConnection::set_url(const std::string & url)
    {
        url_ = url;
    }

    WebSocketConnection::State WebSocketConnection::get_state() const
    {
        return state_;
    }

    void WebSocketConnection::set_state(State s)
    {
        state_ = s;
        if (state_ == State::connected_) {
            mark_connected();
        }
        if (state_ == State::closed_) {
            cancel_close_deadline_timer();
        }
    }

    void WebSocketConnection::mark_connected()
    {
        const uint32_t now = base::time::now();
        last_read_time_ = now;
        last_write_time_ = now;
        last_ping_time_ = 0;
        last_pong_time_ = now;
        pong_deadline_ms_ = 0;
    }

    void WebSocketConnection::use_mask(bool on)
    {
        pkt_parser_.use_mask(on);
    }

    void WebSocketConnection::try_set_heartbeat_timer(NetworkRuntime * runtime)
    {
        runtime_ = runtime;
        if (!config_)
            return;
        uint32_t interval = config_->get_heart_beat_interval();
        if (interval > 0) {
            auto weak = weak_self();
            heartbeat_timer_ = runtime->schedule_periodic(interval, interval, [weak]() {
                if (auto self = weak.lock()) {
                    if (self->conn_ && self->state_ == State::connected_) {
                        self->check_pong_deadline();
                    }
                    if (self->conn_ && self->state_ == State::connected_) {
                        self->send_ping_frame_to(self->conn_);
                    }
                }
            });
        }
    }

    FrameDispatchResult WebSocketConnection::dispatch_frames(Connection * conn)
    {
        mark_read_activity();
        bool close = false;
        auto &chunks = input_chunks_;
        for (size_t i = 0; i < chunks.size(); ++i) {
            auto &chunk = chunks[i];
            if (!chunk.is_completed()) {
                LOG_ERROR("ws internal error: incomplete chunk");
                return FrameDispatchResult::error_;
            }

            if (chunk.head_.is_close_frame()) {
                mark_close_received();
                close = true;
                break;
            } else if (chunk.head_.is_ping_frame()) {
                send_pong_frame_to(conn, chunk.body_);
            } else if (chunk.head_.is_pong_frame()) {
                mark_pong_received();
            } else {
                if (!chunk.body_.empty() || chunk.head_.extend_pay_load_len_ == 0) {
                    if (on_data) {
                        on_data(this, chunk.body_);
                    }
                } else {
                    LOG_ERROR("ws internal error: null body");
                    return FrameDispatchResult::error_;
                }
            }
        }

        clear_input_chunks();

        if (close) {
            return FrameDispatchResult::close_;
        }
        return FrameDispatchResult::ok_;
    }

    FrameDispatchResult WebSocketConnection::dispatch_frames(const std::shared_ptr<Connection> &conn)
    {
        return dispatch_frames(yuan::base::owner_ptr(conn));
    }

    WebSocketHandshaker &WebSocketConnection::handshaker()
    {
        return handshaker_;
    }

    WebSocketPacketParser &WebSocketConnection::pkt_parser()
    {
        return pkt_parser_;
    }

    const std::vector<ProtoChunk> &WebSocketConnection::input_chunks() const
    {
        return input_chunks_;
    }

    std::vector<ProtoChunk> &WebSocketConnection::input_chunks()
    {
        return input_chunks_;
    }

    void WebSocketConnection::clear_input_chunks()
    {
        input_chunks_.clear();
    }

    bool WebSocketConnection::pack_frame(const ::yuan::buffer::ByteBuffer & data, uint8_t type, std::vector< ::yuan::buffer::ByteBuffer> & output)
    {
        auto *orig = get_output_buffers();
        orig->clear();

        if (!pkt_parser_.pack(this, data, type)) {
            return false;
        }

        for (auto &chunk : *orig) {
            output.push_back(std::move(chunk));
        }
        orig->clear();
        return true;
    }

    bool WebSocketConnection::pack_control_frame(const ::yuan::buffer::ByteBuffer & data, uint8_t type, std::vector< ::yuan::buffer::ByteBuffer> & output)
    {
        auto *orig = get_output_buffers();
        orig->clear();

        if (!pkt_parser_.pack_control(this, data, type)) {
            return false;
        }

        for (auto &chunk : *orig) {
            output.push_back(std::move(chunk));
        }
        orig->clear();
        return true;
    }

    void WebSocketConnection::set_outbound_queue_limits(std::size_t max_messages, std::size_t max_bytes)
    {
        outbound_queue_max_messages_ = max_messages;
        outbound_queue_max_bytes_ = max_bytes;
    }

    std::size_t WebSocketConnection::outbound_queue_size() const
    {
        return outbound_queue_.size();
    }

    std::size_t WebSocketConnection::outbound_queue_bytes() const
    {
        return outbound_queue_bytes_;
    }

    bool WebSocketConnection::close_sent() const
    {
        return close_sent_;
    }

    bool WebSocketConnection::close_received() const
    {
        return close_received_;
    }

    bool WebSocketConnection::force_closed() const
    {
        return force_closed_;
    }

    bool WebSocketConnection::close_handshake_expired(uint32_t now_ms) const
    {
        if (close_deadline_ms_ == 0 || state_ == State::closed_) {
            return false;
        }
        if (now_ms == 0) {
            now_ms = base::time::now();
        }
        return now_ms >= close_deadline_ms_;
    }

    uint32_t WebSocketConnection::last_read_time() const
    {
        return last_read_time_;
    }

    uint32_t WebSocketConnection::last_write_time() const
    {
        return last_write_time_;
    }

    uint32_t WebSocketConnection::last_ping_time() const
    {
        return last_ping_time_;
    }

    uint32_t WebSocketConnection::last_pong_time() const
    {
        return last_pong_time_;
    }

    uint32_t WebSocketConnection::max_message_size() const
    {
        return max_message_size_;
    }

    uint32_t WebSocketConnection::max_fragmented_message_size() const
    {
        return max_fragmented_message_size_;
    }

    bool WebSocketConnection::enqueue_output(std::vector< ::yuan::buffer::ByteBuffer> &&output)
    {
        std::size_t added_bytes = 0;
        for (const auto &chunk : output) {
            added_bytes += chunk.readable_bytes();
        }

        if (outbound_queue_max_messages_ > 0 &&
            outbound_queue_.size() + output.size() > outbound_queue_max_messages_) {
            return false;
        }

        if (outbound_queue_max_bytes_ > 0 &&
            outbound_queue_bytes_ + added_bytes > outbound_queue_max_bytes_) {
            return false;
        }

        outbound_queue_.reserve(outbound_queue_.size() + output.size());
        for (auto &chunk : output) {
            outbound_queue_bytes_ += chunk.readable_bytes();
            outbound_queue_.push_back(std::move(chunk));
        }
        return true;
    }

    bool WebSocketConnection::flush_output_queue()
    {
        if (!conn_) {
            return false;
        }

        for (auto &chunk : outbound_queue_) {
            conn_->write(chunk);
        }
        outbound_queue_.clear();
        outbound_queue_bytes_ = 0;
        conn_->flush();
        mark_write_activity();
        return true;
    }

    void WebSocketConnection::mark_close_sent()
    {
        if (close_sent_) {
            return;
        }
        close_sent_ = true;
        state_ = State::closing_;
        const uint32_t timeout = close_handshake_timeout_ms_ == 0 ? DEFAULT_CLOSE_HANDSHAKE_TIMEOUT_MS : close_handshake_timeout_ms_;
        close_deadline_ms_ = base::time::now() + timeout;
        schedule_close_deadline_timer();
    }

    void WebSocketConnection::mark_close_received()
    {
        close_received_ = true;
        state_ = State::closing_;
        if (close_deadline_ms_ == 0) {
            const uint32_t timeout = close_handshake_timeout_ms_ == 0 ? DEFAULT_CLOSE_HANDSHAKE_TIMEOUT_MS : close_handshake_timeout_ms_;
            close_deadline_ms_ = base::time::now() + timeout;
            schedule_close_deadline_timer();
        }
    }

    void WebSocketConnection::mark_read_activity()
    {
        last_read_time_ = base::time::now();
    }

    void WebSocketConnection::mark_write_activity()
    {
        last_write_time_ = base::time::now();
    }

    void WebSocketConnection::mark_ping_sent()
    {
        last_ping_time_ = base::time::now();
        if (config_) {
            const uint32_t timeout = config_->get_heat_beat_timeout();
            pong_deadline_ms_ = timeout > 0 ? last_ping_time_ + timeout : 0;
        }
    }

    void WebSocketConnection::mark_pong_received()
    {
        last_pong_time_ = base::time::now();
        pong_deadline_ms_ = 0;
    }

    void WebSocketConnection::check_pong_deadline()
    {
        if (pong_deadline_ms_ != 0 && base::time::now() >= pong_deadline_ms_) {
            force_close();
        }
    }

    void WebSocketConnection::schedule_close_deadline_timer()
    {
        if (!runtime_ || close_deadline_timer_) {
            return;
        }

        const uint32_t timeout = close_handshake_timeout_ms_ == 0 ? DEFAULT_CLOSE_HANDSHAKE_TIMEOUT_MS : close_handshake_timeout_ms_;
        auto weak = weak_self();
        close_deadline_timer_ = runtime_->schedule(timeout, [weak]() {
            if (auto self = weak.lock()) {
                if (self->state_ == State::closing_) {
                    self->force_close();
                }
            }
        });
    }

    std::weak_ptr<WebSocketConnection> WebSocketConnection::weak_self() noexcept
    {
        return weak_from_this();
    }

    void WebSocketConnection::cancel_close_deadline_timer()
    {
        if (close_deadline_timer_) {
            close_deadline_timer_.cancel();
            close_deadline_timer_.reset();
        }
    }

    void WebSocketConnection::force_close()
    {
        force_closed_ = true;
        state_ = State::closed_;
        cancel_close_deadline_timer();
        if (conn_) {
            conn_->close();
        }
    }

    void WebSocketConnection::send_ping_frame_to(Connection * conn)
    {
        ::yuan::buffer::ByteBuffer empty_payload;
        std::vector< ::yuan::buffer::ByteBuffer> output;
        if (pack_control_frame(empty_payload, static_cast<uint8_t>(OpCodeType::type_ping_frame), output)) {
            for (auto &buf : output) {
                conn->write(buf);
            }
            conn->flush();
            mark_ping_sent();
            mark_write_activity();
        }
    }

    void WebSocketConnection::send_ping_frame_to(const std::shared_ptr<Connection> &conn)
    {
        send_ping_frame_to(yuan::base::owner_ptr(conn));
    }

    void WebSocketConnection::send_pong_frame_to(Connection * conn, const ::yuan::buffer::ByteBuffer & payload)
    {
        std::vector< ::yuan::buffer::ByteBuffer> output;
        if (pack_control_frame(payload, static_cast<uint8_t>(OpCodeType::type_pong_frame), output)) {
            for (auto &buf : output) {
                conn->write(buf);
            }
            conn->flush();
            mark_write_activity();
        }
    }

    void WebSocketConnection::send_pong_frame_to(const std::shared_ptr<Connection> &conn, const ::yuan::buffer::ByteBuffer &payload)
    {
        send_pong_frame_to(yuan::base::owner_ptr(conn), payload);
    }

    void WebSocketConnection::send_close_frame_to(Connection * conn, uint16_t code)
    {
        if (close_sent_) {
            return;
        }
        ::yuan::buffer::ByteBuffer payload(2);
        payload.append_u8(static_cast<uint8_t>((code >> 8) & 0xff));
        payload.append_u8(static_cast<uint8_t>(code & 0xff));
        std::vector< ::yuan::buffer::ByteBuffer> output;
        if (pack_control_frame(payload, static_cast<uint8_t>(OpCodeType::type_close_frame), output)) {
            for (auto &buf : output) {
                conn->write(buf);
            }
            conn->flush();
            mark_write_activity();
            mark_close_sent();
        }
    }

    void WebSocketConnection::send_close_frame_to(const std::shared_ptr<Connection> &conn, uint16_t code)
    {
        send_close_frame_to(yuan::base::owner_ptr(conn), code);
    }

    std::vector< ::yuan::buffer::ByteBuffer> *WebSocketConnection::get_output_buffers()
    {
        return &output_chunks_;
    }

    std::vector<ProtoChunk> *WebSocketConnection::get_input_chunks()
    {
        return &input_chunks_;
    }
}
