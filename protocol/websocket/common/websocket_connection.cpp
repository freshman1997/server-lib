#include "websocket_connection.h"
#include "base/time.h"
#include "handshake.h"
#include "net/connection/connection.h"
#include "net/runtime/network_runtime.h"
#include "timer/timer.h"
#include "websocket_config.h"
#include "websocket_packet_parser.h"
#include "websocket_protocol.h"

#include <cassert>
#include "logger.h"

namespace yuan::net::websocket
{
    WebSocketConnection::WebSocketConnection(WorkMode mode)
        : mode_(mode), state_(State::connecting_), conn_(nullptr), heartbeat_timer_(nullptr), config_(nullptr), last_active_time_(0)
    {
    }

    WebSocketConnection::~WebSocketConnection()
    {
        if (heartbeat_timer_) {
            heartbeat_timer_->cancel();
            heartbeat_timer_ = nullptr;
        }
    }

    void WebSocketConnection::bind_connection(Connection * conn)
    {
        conn_owner_.reset();
        conn_ = conn;
    }

    void WebSocketConnection::bind_connection(const std::shared_ptr<Connection> &conn)
    {
        conn_owner_ = conn;
        conn_ = conn.get();
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
                for (auto &chunk : output) {
                    conn_->write(chunk);
                }
                conn_->flush();
                return true;
            }
            return false;
        }

        if (pkt_parser_.pack(this, buf, (uint8_t)pktType)) {
            for (auto &chunk : output_chunks_) {
                conn_->write(chunk);
            }
            output_chunks_.clear();
            conn_->flush();
            return true;
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
        if (state_ == State::closed_ || state_ == State::closing_) {
            return;
        }
        state_ = State::closing_;
        if (conn_) {
            send_close_frame_to(conn_, (uint16_t)code);
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
    }

    void WebSocketConnection::use_mask(bool on)
    {
        pkt_parser_.use_mask(on);
    }

    void WebSocketConnection::try_set_heartbeat_timer(NetworkRuntime * runtime)
    {
        if (!config_)
            return;
        uint32_t interval = config_->get_heart_beat_interval();
        if (interval > 0) {
            heartbeat_timer_ = runtime->schedule_periodic(interval, interval, [this]() {
                if (conn_ && state_ == State::connected_) {
                    send_ping_frame_to(conn_);
                }
            });
        }
    }

    FrameDispatchResult WebSocketConnection::dispatch_frames(Connection * conn)
    {
        bool close = false;
        auto &chunks = input_chunks_;
        for (size_t i = 0; i < chunks.size(); ++i) {
            auto &chunk = chunks[i];
            if (!chunk.is_completed()) {
                LOG_ERROR("ws internal error: incomplete chunk");
                return FrameDispatchResult::error_;
            }

            if (chunk.head_.is_close_frame()) {
                close = true;
                break;
            } else if (chunk.head_.is_ping_frame()) {
                send_pong_frame_to(conn, chunk.body_);
            } else if (chunk.head_.is_pong_frame()) {
                if (config_) {
                    uint32_t timeout = config_->get_heat_beat_timeout();
                    if (timeout > 0) {
                        if (base::time::now() > last_active_time_ + timeout) {
                            return FrameDispatchResult::close_;
                        }
                    }
                }
                last_active_time_ = base::time::now();
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
        return dispatch_frames(conn.get());
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

    void WebSocketConnection::send_ping_frame_to(Connection * conn)
    {
        ::yuan::buffer::ByteBuffer empty_payload;
        std::vector< ::yuan::buffer::ByteBuffer> output;
        if (pack_control_frame(empty_payload, static_cast<uint8_t>(OpCodeType::type_ping_frame), output)) {
            for (auto &buf : output) {
                conn->write(buf);
            }
            conn->flush();
        }
    }

    void WebSocketConnection::send_ping_frame_to(const std::shared_ptr<Connection> &conn)
    {
        send_ping_frame_to(conn.get());
    }

    void WebSocketConnection::send_pong_frame_to(Connection * conn, const ::yuan::buffer::ByteBuffer & payload)
    {
        std::vector< ::yuan::buffer::ByteBuffer> output;
        if (pack_control_frame(payload, static_cast<uint8_t>(OpCodeType::type_pong_frame), output)) {
            for (auto &buf : output) {
                conn->write(buf);
            }
            conn->flush();
        }
    }

    void WebSocketConnection::send_pong_frame_to(const std::shared_ptr<Connection> &conn, const ::yuan::buffer::ByteBuffer &payload)
    {
        send_pong_frame_to(conn.get(), payload);
    }

    void WebSocketConnection::send_close_frame_to(Connection * conn, uint16_t code)
    {
        ::yuan::buffer::ByteBuffer payload(2);
        payload.append_u8(static_cast<uint8_t>((code >> 8) & 0xff));
        payload.append_u8(static_cast<uint8_t>(code & 0xff));
        std::vector< ::yuan::buffer::ByteBuffer> output;
        if (pack_control_frame(payload, static_cast<uint8_t>(OpCodeType::type_close_frame), output)) {
            for (auto &buf : output) {
                conn->write(buf);
            }
            conn->flush();
        }
    }

    void WebSocketConnection::send_close_frame_to(const std::shared_ptr<Connection> &conn, uint16_t code)
    {
        send_close_frame_to(conn.get(), code);
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
