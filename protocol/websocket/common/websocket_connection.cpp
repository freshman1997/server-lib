#include "websocket_connection.h"
#include "context.h"
#include "handshake.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "packet_parser.h"
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
        ConnData(WebSocketConnection *this_) : mode_(WorkMode::server_), self_(this_), conn_(nullptr), session_(nullptr), handler_(nullptr)
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

                if (session_) {
                    http::HttpSessionContext *ctx = new http::HttpSessionContext(conn);
                    session_= new http::HttpSession((uint64_t)conn, ctx, nullptr);
                }

                auto context = session_->get_context();
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
                if (WebSocketPacketParser::unpack(self_)) {
                    int i = 0;
                    for (; i < input_chunks_.size(); ++i) {
                        if (input_chunks_[i].is_completed()) {
                            handler_->on_receive_packet(self_, input_chunks_[i].body_);
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
                    handler_->on_connected(self_);
                } else {
                    if (session_) {
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
                        handshaker_.on_handshake(context->get_request(), context->get_response());
                        if (!handshaker_.is_handshake_done()) {
                            std::cout << "cant handshake!!!\n";
                            context->process_error(http::ResponseCode::forbidden);
                            return;
                        }
                        handler_->on_connected(self_);
                    }
                }
            }
        }

        virtual void on_write(Connection *conn) {}

        virtual void on_close(Connection *conn)
        {
            if (session_) {
                delete session_;
                session_ = nullptr;
            }
            self_->free_self();
        }

    public:
        void send(Buffer *buff)
        {
            assert(handler_ && conn_);
            if (buff->readable_bytes() > PACKET_MAX_BYTE){
                std::cout << "too long body!!\n";
                conn_->close();
            } else if (WebSocketPacketParser::pack(self_, buff)) {
                for (auto &chunk : output_chunks_) {
                    conn_->write(chunk);
                }
                output_chunks_.clear();

                // flush
                conn_->send();
            }
        }

    public:
        WorkMode mode_;
        WebSocketConnection *self_;
        WebSocketHandler *handler_;
        Connection *conn_;
        http::HttpSession *session_;
        WebSocketHandshaker handshaker_;
        std::vector<ProtoChunk> input_chunks_;
        std::vector<Buffer *> output_chunks_;
    };

    WebSocketConnection::WebSocketConnection(Connection *conn) : data_(std::make_unique<WebSocketConnection::ConnData>(this))
    {
        conn->set_connection_handler(data_.get());
    }

    WebSocketConnection::~WebSocketConnection()
    {
        if (data_->handler_) {
            data_->handler_->on_close(this);
        }
    }

    void WebSocketConnection::send(Buffer *buf)
    {
        assert(data_->conn_ && buf);
        data_->send(buf);
    }

    void WebSocketConnection::close()
    {
        assert(data_->conn_);
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
}
