#include "websocket_connection.h"
#include "context.h"
#include "handshake.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "websocket_packet_parser.h"
#include "response_code.h"
#include "session.h"
#include "websocket_protocol.h"
#include "handler.h"

#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

namespace net::websocket
{
    constexpr uint32_t PACKET_MAX_BYTE = 1024 * 1024;

    class WebSocketConnection::ConnData : public ConnectionHandler
    {
        friend class WebSocketPacketParser;
    public:
        ConnData(WebSocketConnection *this_) : mode_(WorkMode::server_), state_(State::connecting_), self_(this_), conn_(nullptr), session_(nullptr), handler_(nullptr)
        {
        }

    public:
        virtual void on_connected(Connection *conn)
        {
            if (conn_) {
                std::cout << "repeat handshaking!!!\n";
                conn->close();
                return;
            }

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
                }
            } else {
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
        bool send(Buffer *buff)
        {
            assert(handler_ && conn_);
            if (buff->readable_bytes() > PACKET_MAX_BYTE){
                std::cout << "too long body!!\n";
                conn_->close();
            } else if (pktParser_.pack(self_, buff)) {
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

        void send_ping_frame()
        {
            
        }

        void send_pong_frame()
        {

        }

    public:
        WorkMode mode_;
        State state_;
        std::string url_;
        WebSocketConnection *self_;
        WebSocketHandler *handler_;
        Connection *conn_;
        http::HttpSession *session_;
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
        }
    }

    void WebSocketConnection::on_created(Connection *conn)
    {
        data_->on_connected(conn);
    }

    bool WebSocketConnection::send(Buffer *buf)
    {
        assert(data_->conn_ && buf);
        return data_->send(buf);
    }

    void WebSocketConnection::close()
    {
        assert(data_->conn_);
        data_->state_ = State::closing;
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
}
