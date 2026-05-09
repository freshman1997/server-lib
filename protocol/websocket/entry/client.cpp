#include "client.h"
#include "net/connection/connection.h"
#include "../common/websocket_connection.h"
#include "../common/websocket_config.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/inet_address.h"
#include "../common/close_code.h"
#include "context.h"
#include "session.h"
#include "response_code.h"
#include "net/security/ssl_module.h"
#include <memory>

#if defined(WS_USE_SSL)
#include "net/security/openssl.h"
#include "net/connection/stream_transport.h"
#include "coroutine/stream_io_awaitable.h"
#endif

#include <cassert>
#include "logger.h"

namespace yuan::net::websocket
{

    namespace
    {
        template <typename T>
        T *ptr_of(const std::shared_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }

        template <typename T>
        T *ptr_of(const std::unique_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }
    }

    struct WebSocketClient::ClientData
    {
        WebSocketDataHandler *data_handler_ = nullptr;
        WebSocketConfigManager config_;
        std::shared_ptr<SSLModule> ssl_module_;
        std::unique_ptr<NetworkRuntime> owned_runtime_;
        net::AsyncClientSession session_;
    };

    WebSocketClient::WebSocketClient()
        : data_(std::make_unique<WebSocketClient::ClientData>())
    {
    }

    WebSocketClient::~WebSocketClient()
    {
        exit();
    }

    bool WebSocketClient::init()
    {
        if (!data_->config_.init(false)) {
            return false;
        }

#if defined(WS_USE_SSL)
        data_->ssl_module_ = std::make_shared<OpenSSLModule>();
        if (!data_->ssl_module_->init("./ca/ca.crt")) {
            if (auto msg = data_->ssl_module_->get_error_message()) {
                LOG_ERROR("{}", msg->c_str());
            }
            return false;
        }
#endif

        data_->owned_runtime_ = std::make_unique<NetworkRuntime>();
        return true;
    }

    bool WebSocketClient::connect(const InetAddress & addr, const std::string & url)
    {
        auto task = run_async(addr, url);
        task.resume();
        task.detach();
        return true;
    }

    void WebSocketClient::set_data_handler(WebSocketDataHandler * handler)
    {
        data_->data_handler_ = handler;
    }

    void WebSocketClient::run()
    {
        assert(data_->owned_runtime_);
        data_->owned_runtime_->run();
    }

    void WebSocketClient::exit()
    {
        if (data_->session_.is_connected()) {
            data_->session_.close();
        }
        data_->owned_runtime_.reset();
    }

    coroutine::Task<void> WebSocketClient::run_async(const InetAddress & addr, const std::string & url)
    {
        auto rv = data_->owned_runtime_->runtime_view();

        bool ok = co_await data_->session_.connect_async(rv, addr.get_ip(), addr.get_port());
        if (!ok) {
            if (data_->data_handler_) {
                data_->data_handler_->on_close(nullptr);
            }
            co_return;
        }

        auto conn = data_->session_.context().connection();

#if defined(WS_USE_SSL)
        if (data_->ssl_module_) {
            auto *stream = dynamic_cast<StreamTransport *>(ptr_of(conn));
            auto *channel = stream ? stream->stream_channel() : nullptr;
            if (!channel) {
                if (data_->data_handler_) {
                    data_->data_handler_->on_close(nullptr);
                }
                co_return;
            }

            auto sslHandler = data_->ssl_module_->create_handler(channel->get_fd(), SSLHandler::SSLMode::connector_);
            if (!sslHandler) {
                if (data_->data_handler_) {
                    data_->data_handler_->on_close(nullptr);
                }
                co_return;
            }

            conn->set_ssl_handler(sslHandler);

            auto handshake_result = co_await coroutine::async_ssl_handshake(rv, conn, 0);
            if (handshake_result != coroutine::SslHandshakeResult::success) {
                if (data_->data_handler_) {
                    data_->data_handler_->on_close(nullptr);
                }
                co_return;
            }
        }
#endif

        WebSocketConnection wsConn(WebSocketConnection::WorkMode::client_);
        wsConn.bind_connection(conn);
        wsConn.set_config(&data_->config_);
        wsConn.set_url(url);

        if (data_->data_handler_) {
            wsConn.on_data = [this](WebSocketConnection *c, const ::yuan::buffer::ByteBuffer &buf) {
                data_->data_handler_->on_data(c, buf);
            };
            wsConn.on_connected_cb = [this](WebSocketConnection *c) {
                data_->data_handler_->on_connected(c);
            };
            wsConn.on_close_cb = [this](WebSocketConnection *c) {
                data_->data_handler_->on_close(c);
                if (data_->owned_runtime_) {
                    data_->owned_runtime_->stop();
                }
            };
        }

        {
            http::HttpSessionContext httpCtx(conn);
            httpCtx.set_mode(http::Mode::client);
            httpCtx.get_request()->set_raw_url(url);

            if (!wsConn.handshaker().on_handshake(httpCtx.get_request(), httpCtx.get_response(),
                                                  WebSocketConnection::WorkMode::client_)) {
                LOG_WARN("cant handshake!");
                data_->session_.close();
                if (data_->data_handler_) {
                    data_->data_handler_->on_close(nullptr);
                }
                co_return;
            }

            co_await data_->session_.flush_async();

            while (!wsConn.handshaker().is_handshake_done()) {
                auto read_result = co_await data_->session_.read_async();
                if (read_result.status != coroutine::IoStatus::success) {
                    if (data_->data_handler_) {
                        data_->data_handler_->on_close(nullptr);
                    }
                    co_return;
                }

                if (!httpCtx.parse_from(read_result.data)) {
                    data_->session_.close();
                    if (data_->data_handler_) {
                        data_->data_handler_->on_close(nullptr);
                    }
                    co_return;
                }

                if (httpCtx.has_error()) {
                    data_->session_.close();
                    if (data_->data_handler_) {
                        data_->data_handler_->on_close(nullptr);
                    }
                    co_return;
                }

                if (httpCtx.is_completed()) {
                    wsConn.handshaker().on_handshake(httpCtx.get_request(), httpCtx.get_response(),
                                                     WebSocketConnection::WorkMode::client_, true);
                    if (!wsConn.handshaker().is_handshake_done()) {
                        LOG_WARN("cant handshake!");
                        data_->session_.close();
                        if (data_->data_handler_) {
                            data_->data_handler_->on_close(nullptr);
                        }
                        co_return;
                    }
                }
            }

            auto leftover = httpCtx.take_leftover_buffer();
            if (!leftover.empty()) {
                if (!wsConn.pkt_parser().unpack_from(&wsConn, leftover)) {
                    data_->session_.close();
                    if (data_->data_handler_) {
                        data_->data_handler_->on_close(nullptr);
                    }
                    co_return;
                }
            }
        }

        wsConn.set_state(WebSocketConnection::State::connected_);
        wsConn.try_set_heartbeat_timer(ptr_of(data_->owned_runtime_));
        if (wsConn.on_connected_cb) {
            wsConn.on_connected_cb(&wsConn);
        }

        if (!wsConn.input_chunks().empty()) {
            auto result = wsConn.dispatch_frames(conn);
            if (result == FrameDispatchResult::close_) {
                wsConn.send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::normal_close_);
                data_->session_.close();
            } else if (result == FrameDispatchResult::error_) {
                wsConn.send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::invalid_palyload_);
                data_->session_.close();
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
            auto read_result = co_await data_->session_.read_async();
            if (read_result.status != coroutine::IoStatus::success) {
                break;
            }

            if (!wsConn.pkt_parser().unpack_from(&wsConn, read_result.data)) {
                wsConn.send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::invalid_palyload_);
                data_->session_.close();
                break;
            }

            auto result = wsConn.dispatch_frames(conn);
            if (result == FrameDispatchResult::close_) {
                wsConn.send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::normal_close_);
                data_->session_.close();
                break;
            } else if (result == FrameDispatchResult::error_) {
                wsConn.send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::invalid_palyload_);
                data_->session_.close();
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
