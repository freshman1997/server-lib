#include "client.h"
#include "net/connection/connection.h"
#include "../common/websocket_connection.h"
#include "../common/websocket_config.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/inet_address.h"
#include "../common/close_code.h"
#include "base/owner_ptr.h"
#include "context.h"
#include "session.h"
#include "response_code.h"
#include "net/security/ssl_module.h"
#include <memory>
#include <mutex>

#if defined(WS_USE_SSL)
#include "net/security/openssl.h"
#include "net/connection/stream_transport.h"
#include "coroutine/stream_io_awaitable.h"
#endif

#include <cassert>
#include "logger.h"

namespace yuan::net::websocket
{
    struct WebSocketClient::ClientData
    {
        WebSocketDataHandler *data_handler_ = nullptr;
        WebSocketConfigManager config_;
        std::shared_ptr<SSLModule> ssl_module_;
        std::unique_ptr<NetworkRuntime> owned_runtime_;
        net::AsyncClientSession session_;
        std::mutex ws_mutex_;
        std::shared_ptr<WebSocketConnection> ws_connection_;
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
        if (!data_->ssl_module_->init(data_->config_.get_tls_cert_path())) {
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
        std::shared_ptr<WebSocketConnection> ws_connection;
        {
            std::lock_guard<std::mutex> lock(data_->ws_mutex_);
            ws_connection = std::move(data_->ws_connection_);
        }
        if (ws_connection) {
            ws_connection->shutdown();
        }
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
            auto *stream = dynamic_cast<StreamTransport *>(yuan::base::owner_ptr(conn));
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

        auto wsConn = std::make_shared<WebSocketConnection>(WebSocketConnection::WorkMode::client_);
        wsConn->bind_connection(conn);
        wsConn->set_config(&data_->config_);
        wsConn->set_url(url);
        {
            std::lock_guard<std::mutex> lock(data_->ws_mutex_);
            data_->ws_connection_ = wsConn;
        }
        auto session_guard = std::shared_ptr<void>(wsConn.get(), [this, wsConn](void *) {
            std::lock_guard<std::mutex> lock(data_->ws_mutex_);
            if (data_->ws_connection_ == wsConn) {
                data_->ws_connection_.reset();
            }
        });

        if (data_->data_handler_) {
            wsConn->on_data = [this](WebSocketConnection *c, const ::yuan::buffer::ByteBuffer &buf) {
                data_->data_handler_->on_data(c, buf);
            };
            wsConn->on_connected_cb = [this](WebSocketConnection *c) {
                data_->data_handler_->on_connected(c);
            };
            wsConn->on_close_cb = [this](WebSocketConnection *c) {
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
            const uint32_t handshake_timeout = data_->config_.get_handshake_timeout();

            if (!wsConn->handshaker().on_handshake(httpCtx.get_request(), httpCtx.get_response(),
                                                   WebSocketConnection::WorkMode::client_)) {
                LOG_WARN("cant handshake!");
                data_->session_.close();
                if (data_->data_handler_) {
                    data_->data_handler_->on_close(nullptr);
                }
                co_return;
            }

            co_await data_->session_.flush_async();

            while (!wsConn->handshaker().is_handshake_done()) {
                auto read_result = co_await data_->session_.read_async(handshake_timeout);
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
                    wsConn->handshaker().on_handshake(httpCtx.get_request(), httpCtx.get_response(),
                                                      WebSocketConnection::WorkMode::client_, true);
                    if (!wsConn->handshaker().is_handshake_done()) {
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
                if (!wsConn->pkt_parser().unpack_from(wsConn.get(), leftover)) {
                    data_->session_.close();
                    if (data_->data_handler_) {
                        data_->data_handler_->on_close(nullptr);
                    }
                    co_return;
                }
            }
        }

        wsConn->set_state(WebSocketConnection::State::connected_);
        wsConn->try_set_heartbeat_timer(yuan::base::owner_ptr(data_->owned_runtime_));
        if (wsConn->on_connected_cb) {
            wsConn->on_connected_cb(wsConn.get());
        }

        if (!wsConn->input_chunks().empty()) {
            auto result = wsConn->dispatch_frames(conn);
            if (result == FrameDispatchResult::close_) {
                wsConn->send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::normal_close_);
                data_->session_.close();
            } else if (result == FrameDispatchResult::error_) {
                wsConn->send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::invalid_palyload_);
                data_->session_.close();
            }
            if (result != FrameDispatchResult::ok_) {
                wsConn->set_state(WebSocketConnection::State::closed_);
                if (wsConn->on_close_cb) {
                    wsConn->on_close_cb(wsConn.get());
                }
                co_return;
            }
        }

        const uint32_t read_idle_timeout = data_->config_.get_read_idle_timeout();
        while (wsConn->connected()) {
            auto read_result = co_await data_->session_.read_async(read_idle_timeout);
            if (read_result.status != coroutine::IoStatus::success) {
                break;
            }

            if (!wsConn->pkt_parser().unpack_from(wsConn.get(), read_result.data)) {
                wsConn->send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::invalid_palyload_);
                data_->session_.close();
                break;
            }

            auto result = wsConn->dispatch_frames(conn);
            if (result == FrameDispatchResult::close_) {
                wsConn->send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::normal_close_);
                data_->session_.close();
                break;
            } else if (result == FrameDispatchResult::error_) {
                wsConn->send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::invalid_palyload_);
                data_->session_.close();
                break;
            }
        }

        wsConn->set_state(WebSocketConnection::State::closed_);
        if (wsConn->on_close_cb) {
            wsConn->on_close_cb(wsConn.get());
        }

        co_return;
    }
}
