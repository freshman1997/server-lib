#include "websocket_connection.h"
#include "base/time.h"
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
        ConnData(WebSocketConnection *this_, WorkMode mode) : mode_(mode), state_(State::connecting_), self_(this_), conn_(nullptr), session_(nullptr), handler_(nullptr), heartbeat_timer_(nullptr)
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
                    ctx->set_mode(http::Mode::client);
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
                if (pkt_parser_.unpack(self_)) {
                    bool close = false;
                    for (int i = 0; i < input_chunks_.size(); ++i) {
                        auto *chunk = &input_chunks_[i];
                        if (chunk->is_completed()) {
                            if (chunk->head_.is_close_frame()) {
                                close = true;
                                break;
                            } else if (chunk->head_.is_ping_frame()) {
                                send_pong_frame();
                            } else if (chunk->head_.is_pong_frame()) {
                                on_pong_frame();
                            } else {
                                if (chunk->body_) {
                                    handler_->on_receive_packet(self_, chunk->body_);
                                } else {
                                    std::cout << "internal error occured !!\n";
                                    close = true;
                                    break;
                                }
                            }
                        } else {
                            if (chunk->body_) {
                                if (!chunk->head_.is_fin()) {
                                    reserve_list_.push_back(*chunk);
                                    continue;
                                }
                                std::cout << "internal error occured !!\n";
                                close = true;
                            }
                            break;
                        }
                    }

                    for (int i = 0; i < input_chunks_.size(); ++i) {
                        BufferedPool::get_instance()->free(input_chunks_[i].body_);
                    }

                    input_chunks_.clear();

                    if (!reserve_list_.empty()) {
                        input_chunks_ = reserve_list_;
                        reserve_list_.clear();
                    }

                    if (close) {
                        conn->close();
                        return;
                    }
                } else {
                    send_close_frame();
                    conn->close();
                }
            } else {
                assert(state_ == State::connecting_);
                if (!session_ && mode_ == WorkMode::server_) {
                    http::HttpSessionContext *ctx = new http::HttpSessionContext(conn);
                    ctx->set_mode(http::Mode::server);
                    session_= new http::HttpSession((uint64_t)conn, ctx, nullptr);
                }

                auto context = session_->get_context();
                assert(!context->is_completed());

                if (!context->parse()) {
                    if (mode_ == WorkMode::server_) {
                        if (context->has_error()) {
                            context->process_error(context->get_error_code());
                        }
                    } else {
                        conn->close();
                    }
                    return;
                }

                if (context->has_error()) {
                    context->process_error(context->get_error_code());
                    return;
                }

                if (context->is_completed()) {
                    if (mode_ == WorkMode::client_) {
                        handshaker_.on_handshake(context->get_request(), context->get_response(), WorkMode::client_, true);
                        if (!handshaker_.is_handshake_done()) {
                            std::cout << "cant handshake!!!\n";
                            conn->close();
                            return;
                        }
                    } else {
                        handshaker_.on_handshake(context->get_request(), context->get_response(), WorkMode::server_);
                        if (!handshaker_.is_handshake_done()) {
                            std::cout << "cant handshake!!!\n";
                            context->process_error(http::ResponseCode::forbidden);
                            return;
                        }
                        url_ = context->get_request()->get_raw_url();
                    }
                    
                    state_ = State::connected_;
                    handler_->on_connected(self_);

                    delete session_;
                    session_ = nullptr;
                }
            }
        }

        virtual void on_write(Connection *conn) {}

        virtual void on_close(Connection *conn)
        {
            state_ = State::closed_;
            if (session_) {
                delete session_;
                session_ = nullptr;
            }
            self_->free_self();
        }

    public:
        bool send(Buffer *buff, PacketType pktType)
        {
            if (state_ != State::connected_) {
                return false;
            }

            if (pktType == PacketType::close_ || pktType == PacketType::ping_ || pktType == PacketType::pong_) {
                conn_->write_and_flush(buff);
                return true;
            }

            assert(handler_ && conn_);
            if (pkt_parser_.pack(self_, buff, (uint8_t)pktType)) {
                for (auto &chunk : output_chunks_) {
                    conn_->write(chunk);
                }
                output_chunks_.clear();

                // flush
                conn_->send();
                BufferedPool::get_instance()->free(buff);

                return true;
            } else {
                BufferedPool::get_instance()->free(buff);
                std::cerr << "cant pack ws frame!!\n";
                conn_->close();
                return false;
            }
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
            state_ = State::closing_;
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

        void on_pong_frame()
        {
            last_active_time_ = base::time::now();
            // TODO
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
        WebSocketPacketParser pkt_parser_;
        std::vector<ProtoChunk> input_chunks_;
        std::vector<Buffer *> output_chunks_;
        std::vector<ProtoChunk> reserve_list_;;
        uint32_t last_active_time_;
    };

    WebSocketConnection::WebSocketConnection(WorkMode mode) : data_(std::make_unique<WebSocketConnection::ConnData>(this, mode))
    {
        if (mode == WorkMode::client_) {
            use_mask(WebSocketConfigManager::get_instance()->is_client_use_mask());
        } else {
            use_mask(WebSocketConfigManager::get_instance()->is_server_use_mask());
        }
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

    bool WebSocketConnection::send(const char *data, size_t len, PacketType pktType)
    {
        if (data_->state_ != State::connected_) {
            return false;
        }
        
        Buffer *buff = BufferedPool::get_instance()->allocate(len);
        buff->write_string(data, len);

        bool res = send(buff, pktType);
        if (!res) {
            BufferedPool::get_instance()->free(buff);
        }

        return res;
    }

    void WebSocketConnection::close()
    {
        assert(data_->conn_);
        data_->state_ = State::closing_;
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

    void WebSocketConnection::use_mask(bool on)
    {
        data_->pkt_parser_.use_mask(on);
    }
}
