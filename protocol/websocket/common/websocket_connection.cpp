#include "websocket_connection.h"
#include "buffer/pool.h"
#include "context.h"
#include "handshake.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "timer/timer.h"
#include "websocket_config.h"
#include "websocket_packet_parser.h"
#include "response_code.h"
#include "session.h"
#include "websocket_protocol.h"
#include "timer/timer_util.hpp"
#include "handler.h"

#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

namespace net::websocket
{
    class WebSocketConnection::ConnData : public ConnectionHandler
    {
        friend class WebSocketPacketParser;
    public:
        ConnData(WebSocketConnection *this_) : mode_(WorkMode::server_), state_(State::connecting_), self_(this_), conn_(nullptr), session_(nullptr), handler_(nullptr), heartbeat_timer_(nullptr)
        {
        }

        ~ConnData()
        {
            if (heartbeat_timer_) {
                heartbeat_timer_->cancel();
                heartbeat_timer_ = nullptr;
            }

            if (session_) {
                delete session_;
                session_ = nullptr;
            }

            for (auto &item : input_chunks_) {
                if (item.body_) {
                    BufferedPool::get_instance()->free(item.body_);
                    item.body_ = nullptr;
                }
            }
            input_chunks_.clear();

            for (const auto &item : output_chunks_) {
                BufferedPool::get_instance()->free(item);
            }
            output_chunks_.clear();
        }

    public:
        virtual void on_connected(Connection *conn)
        {
            if (conn_) {
                std::cout << "repeat handshaking!!!\n";
                conn->close();
                return;
            }

            assert(state_ == State::connecting_);

            conn_ = conn;
            conn_->set_connection_handler(this);

            if (mode_ == WorkMode::client_) {
                assert(!handshaker_.is_handshake_done());

                if (url_.empty()) {
                    url_ = "/";
                }

                if (!session_) {
                    http::HttpSessionContext *ctx = new http::HttpSessionContext(conn);
                    session_= new http::HttpSession((uint64_t)conn, ctx, nullptr);
                }

                auto context = session_->get_context();
                context->get_request()->set_raw_url(url_);
                if (!handshaker_.on_handshake(context->get_request(), context->get_response(), WorkMode::client_)) {
                    std::cout << "cant handshake!!!\n";
                    conn->close();
                }
            }
        }

        virtual void on_error(Connection *conn) {}

        virtual void on_read(Connection *conn)
        {
            if (handshaker_.is_handshake_done()) {
                assert(handler_);
                if (pktParser_.unpack(self_)) {
                    int i = 0;
                    for (; i < input_chunks_.size(); ++i) {
                        auto *chunk = &input_chunks_[i];
                        if (chunk->is_completed()) {
                            if (chunk->head_.is_close_frame()) {
                                conn->close();
                                return;
                            } else if (chunk->head_.is_ping_frame()) {
                                send_pong_frame();
                            } else {
                                handler_->on_receive_packet(self_, input_chunks_[i].body_);
                            }
                        } else if (input_chunks_[i].body_ && input_chunks_[i].body_->readable_bytes() > PACKET_MAX_BYTE){
                            std::cout << "too long body!!\n";
                            conn->close();
                        }
                    }
                    input_chunks_.clear();
                }
            } else {
                assert(state_ == State::connecting_);
                if (mode_ == WorkMode::client_) {
                    assert(session_ && handler_);
                    auto context = session_->get_context();
                    handshaker_.on_handshake(context->get_request(), context->get_response(), WorkMode::client_, true);
                    if (!handshaker_.is_handshake_done()) {
                        std::cout << "cant handshake!!!\n";
                        conn->close();
                        return;
                    }
                    state_ = State::connected;
                    handler_->on_connected(self_);
                } else {
                    if (!session_) {
                        http::HttpSessionContext *ctx = new http::HttpSessionContext(conn);
                        session_= new http::HttpSession((uint64_t)conn, ctx, nullptr);
                    }

                    auto context = session_->get_context();
                    assert(!context->is_completed());

                    if (!context->parse()) {
                        if (context->has_error()) {
                            context->process_error(context->get_error_code());
                        }
                        return;
                    }

                    if (context->has_error()) {
                        context->process_error(context->get_error_code());
                        return;
                    }

                    if (context->is_completed()) {
                        handshaker_.on_handshake(context->get_request(), context->get_response(), WorkMode::server_);
                        if (!handshaker_.is_handshake_done()) {
                            std::cout << "cant handshake!!!\n";
                            context->process_error(http::ResponseCode::forbidden);
                            return;
                        }
                        state_ = State::connected;
                        url_ = context->get_request()->get_raw_url();
                        handler_->on_connected(self_);
                    }
                }
            }
        }

        virtual void on_write(Connection *conn) {}

        virtual void on_close(Connection *conn)
        {
            state_ = State::closed;
            if (session_) {
                delete session_;
                session_ = nullptr;
            }
            self_->free_self();
        }

    public:
        bool send(Buffer *buff, PacketType pktType)
        {
            if (state_ != State::connected) {
                return false;
            }

            if (pktType == PacketType::close_ || pktType == PacketType::ping_ || pktType == PacketType::pong_) {
                output_chunks_.push_back(buff);
                return true;
            }

            assert(handler_ && conn_);
            if (pktParser_.pack(self_, buff, (uint8_t)pktType)) {
                for (auto &chunk : output_chunks_) {
                    conn_->write(chunk);
                }
                output_chunks_.clear();

                // flush
                conn_->send();

                return true;
            } else {
                std::cerr << "cant pack ws frame!!\n";
                conn_->close();
            }

            return false;
        }

        void hear_beat(timer::Timer *timer)
        {
            send_ping_frame();
        }

        void send_ping_frame()
        {
            Buffer *buf = BufferedPool::get_instance()->allocate(5);
            buf->write_uint8(0x89);
            buf->write_uint8(0x03);
            buf->write_uint8(0x00);
            buf->write_uint8(0x01);
            buf->write_uint8(0x02);
            send(buf, PacketType::ping_);
        }

        void send_pong_frame()
        {
            Buffer *buf = BufferedPool::get_instance()->allocate(5);
            buf->write_uint8(0x8a);
            buf->write_uint8(0x03);
            buf->write_uint8(0x00);
            buf->write_uint8(0x01);
            buf->write_uint8(0x02);
            send(buf, PacketType::pong_);
        }

        void send_close_frame()
        {
            Buffer *buf = BufferedPool::get_instance()->allocate(4);
            buf->write_uint8(0x88);
            buf->write_uint8(0x02);

            // 这里表示关闭连接的原因码为 1000
            buf->write_uint8(0x03);
            buf->write_uint8(0xE8);
            send(buf, PacketType::close_);
        }

    public:
        WorkMode mode_;
        State state_;
        std::string url_;
        WebSocketConnection *self_;
        WebSocketHandler *handler_;
        Connection *conn_;
        http::HttpSession *session_;
        timer::Timer *heartbeat_timer_;
        WebSocketHandshaker handshaker_;
        WebSocketPacketParser pktParser_;
        std::vector<ProtoChunk> input_chunks_;
        std::vector<Buffer *> output_chunks_;
    };

    WebSocketConnection::WebSocketConnection() : data_(std::make_unique<WebSocketConnection::ConnData>(this))
    {
        
    }

    WebSocketConnection::~WebSocketConnection()
    {
        if (data_->handler_) {
            data_->handler_->on_close(this);
            data_->handler_ = nullptr;
        }
    }

    void WebSocketConnection::on_created(Connection *conn)
    {
        data_->on_connected(conn);
    }

    bool WebSocketConnection::send(Buffer *buf, PacketType pktType)
    {
        assert(data_->conn_ && buf);
        return data_->send(buf, pktType);
    }

    void WebSocketConnection::close()
    {
        assert(data_->conn_);
        data_->state_ = State::closing;
        data_->send_close_frame();
        data_->conn_->close();
    }

    void WebSocketConnection::set_handler(WebSocketHandler *handler)
    {
        data_->handler_ = handler;
    }

    void WebSocketConnection::free_self()
    {
        delete this;
    }

    net::Connection * WebSocketConnection::get_native_connection()
    {
        return data_->conn_;
    }

    std::vector<Buffer *> * WebSocketConnection::get_output_buffers()
    {
        return &data_->output_chunks_;
    }

    std::vector<ProtoChunk> * WebSocketConnection::get_input_chunks()
    {
        return &data_->input_chunks_;
    }

    const std::string & WebSocketConnection::get_url() const
    {
        return data_->url_;
    }

    void WebSocketConnection::set_url(const std::string &url)
    {
        data_->url_ = url;
    }

    WebSocketConnection::State WebSocketConnection::get_state() const
    {
        return data_->state_;
    }

    void WebSocketConnection::try_set_heartbeat_timer(timer::TimerManager *timerManager)
    {
        uint32_t interval = WebSocketConfigManager::get_instance()->get_heart_beat_interval();
        if (interval > 0) {
            data_->heartbeat_timer_ = timer::TimerUtil::build_period_timer(timerManager, interval, interval, this->data_.get(), &WebSocketConnection::ConnData::hear_beat);
        }
    }
}
