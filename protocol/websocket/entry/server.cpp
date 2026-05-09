#include "server.h"
#include "net/connection/connection.h"
#include "net/async/async_connection_context.h"
#include "../common/websocket_connection.h"
#include "data_handler.h"
#include "../common/websocket_config.h"
#include "../common/close_code.h"
#include "context.h"
#include "session.h"
#include "response_code.h"
#include "net/security/ssl_module.h"

#if defined(WS_USE_SSL)
#include "net/security/openssl.h"
#endif

#include <memory>

#include "logger.h"

namespace yuan::net::websocket
{
    struct WebSocketServer::ServerData
    {
        WebSocketDataHandler *data_handler_ = nullptr;
        WebSocketConfigManager config_;
        std::shared_ptr<SSLModule> ssl_module_;
        net::AsyncListenerHost listener_;
        std::unique_ptr<NetworkRuntime> owned_runtime_;
    };

    WebSocketServer::WebSocketServer()
        : data_(std::make_unique<WebSocketServer::ServerData>())
    {
    }

    WebSocketServer::~WebSocketServer()
    {
    }

    bool WebSocketServer::init(int port)
    {
#if defined(WS_USE_SSL)
        data_->ssl_module_ = std::make_shared<OpenSSLModule>();
        if (!data_->ssl_module_->init("./ca/ca.crt", "./ca/ca.key", SSLHandler::SSLMode::acceptor_)) {
            if (auto msg = data_->ssl_module_->get_error_message()) {
                LOG_ERROR("{}", msg->c_str());
            }
            return false;
        }
        data_->listener_.set_ssl_module(data_->ssl_module_);
#endif

        data_->owned_runtime_ = std::make_unique<NetworkRuntime>();
        if (!data_->listener_.bind(port, *data_->owned_runtime_)) {
            data_->owned_runtime_.reset();
            return false;
        }

        return data_->config_.init(true);
    }

    bool WebSocketServer::init(int port, NetworkRuntime & runtime)
    {
#if defined(WS_USE_SSL)
        data_->ssl_module_ = std::make_shared<OpenSSLModule>();
        if (!data_->ssl_module_->init("./ca/ca.crt", "./ca/ca.key", SSLHandler::SSLMode::acceptor_)) {
            if (auto msg = data_->ssl_module_->get_error_message()) {
                LOG_ERROR("{}", msg->c_str());
            }
            return false;
        }
        data_->listener_.set_ssl_module(data_->ssl_module_);
#endif

        if (!data_->listener_.bind(port, runtime)) {
            return false;
        }

        return data_->config_.init(true);
    }

    void WebSocketServer::serve()
    {
        data_->listener_.set_connection_handler(
            [this](net::AsyncConnectionContext ctx)->coroutine::Task<void> {
                co_await handle_connection(std::move(ctx));
            });

        if (data_->owned_runtime_) {
            auto accept_task = data_->listener_.run_async();
            accept_task.resume();
            data_->owned_runtime_->run();
        }
    }

    void WebSocketServer::set_data_handler(WebSocketDataHandler * handler)
    {
        data_->data_handler_ = handler;
    }

    void WebSocketServer::stop()
    {
        if (data_->owned_runtime_) {
            data_->owned_runtime_->stop();
        }
    }

    coroutine::Task<void> WebSocketServer::handle_connection(net::AsyncConnectionContext ctx)
    {
        auto conn = ctx.connection();

        if (conn && conn->is_ssl_handshaking()) {
            auto hs_result = co_await ctx.ssl_handshake_async();
            if (hs_result != coroutine::SslHandshakeResult::success) {
                co_return;
            }
        }

        WebSocketConnection wsConn(WebSocketConnection::WorkMode::server_);
        wsConn.bind_connection(conn);
        wsConn.set_config(&data_->config_);

        if (data_->data_handler_) {
            wsConn.on_data = [this](WebSocketConnection *c, const ::yuan::buffer::ByteBuffer &buf) {
                data_->data_handler_->on_data(c, buf);
            };
            wsConn.on_connected_cb = [this](WebSocketConnection *c) {
                c->try_set_heartbeat_timer(data_->listener_.runtime());
                data_->data_handler_->on_connected(c);
            };
            wsConn.on_close_cb = [this](WebSocketConnection *c) {
                data_->data_handler_->on_close(c);
            };
        }

        {
            http::HttpSessionContext httpCtx(conn);
            httpCtx.set_mode(http::Mode::server);

            while (!wsConn.handshaker().is_handshake_done()) {
                auto read_result = co_await ctx.read_async();
                if (read_result.status != coroutine::IoStatus::success) {
                    co_return;
                }

                if (!httpCtx.parse_from(read_result.data)) {
                    if (httpCtx.has_error()) {
                        httpCtx.process_error(httpCtx.get_error_code());
                    }
                    co_return;
                }

                if (httpCtx.has_error()) {
                    httpCtx.process_error(httpCtx.get_error_code());
                    co_return;
                }

                if (httpCtx.is_completed()) {
                    wsConn.handshaker().on_handshake(
                        httpCtx.get_request(), httpCtx.get_response(),
                        WebSocketConnection::WorkMode::server_);
                    if (!wsConn.handshaker().is_handshake_done()) {
                        httpCtx.process_error(http::ResponseCode::forbidden);
                        co_return;
                    }
                    wsConn.set_url(httpCtx.get_request()->get_raw_url());
                    wsConn.set_state(WebSocketConnection::State::connected_);
                    if (wsConn.on_connected_cb) {
                        wsConn.on_connected_cb(&wsConn);
                    }
                }
            }

            auto leftover = httpCtx.take_leftover_buffer();
            if (!leftover.empty()) {
                if (!wsConn.pkt_parser().unpack_from(&wsConn, leftover)) {
                    wsConn.send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::invalid_palyload_);
                    ctx.close();
                    wsConn.set_state(WebSocketConnection::State::closed_);
                    if (wsConn.on_close_cb) {
                        wsConn.on_close_cb(&wsConn);
                    }
                    co_return;
                }
            }
        }

        if (!wsConn.input_chunks().empty()) {
            auto result = wsConn.dispatch_frames(conn);
            if (result == FrameDispatchResult::close_) {
                wsConn.send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::normal_close_);
                ctx.close();
            } else if (result == FrameDispatchResult::error_) {
                wsConn.send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::invalid_palyload_);
                ctx.close();
            }
            if (result != FrameDispatchResult::ok_) {
                wsConn.set_state(WebSocketConnection::State::closed_);
                if (wsConn.on_close_cb) {
                    wsConn.on_close_cb(&wsConn);
                }
                co_return;
            }
        }

        while (wsConn.connected()) {
            auto read_result = co_await ctx.read_async();
            if (read_result.status != coroutine::IoStatus::success) {
                break;
            }

            if (!wsConn.pkt_parser().unpack_from(&wsConn, read_result.data)) {
                wsConn.send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::invalid_palyload_);
                ctx.close();
                break;
            }

            auto result = wsConn.dispatch_frames(conn);
            if (result == FrameDispatchResult::close_) {
                wsConn.send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::normal_close_);
                ctx.close();
                break;
            } else if (result == FrameDispatchResult::error_) {
                wsConn.send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::invalid_palyload_);
                ctx.close();
                break;
            }
        }

        wsConn.set_state(WebSocketConnection::State::closed_);
        if (wsConn.on_close_cb) {
            wsConn.on_close_cb(&wsConn);
        }

        co_return;
    }
}
