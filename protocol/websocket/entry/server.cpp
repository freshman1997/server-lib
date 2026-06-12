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
#include "net/socket/listen_options.h"
#include "base/time.h"

#if defined(WS_USE_SSL)
#include "net/security/openssl.h"
#endif

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
        std::mutex sessions_mutex_;
        std::unordered_set<std::shared_ptr<WebSocketConnection>> sessions_;

        struct RateState
        {
            uint32_t window_start_ms = 0;
            uint32_t count = 0;
        };

        std::mutex rate_mutex_;
        std::unordered_map<std::string, RateState> handshake_rate_;
        std::unordered_map<std::string, RateState> message_rate_;

        bool allow_rate(std::unordered_map<std::string, RateState> &states,
                        const std::string &ip,
                        uint32_t max_count,
                        uint32_t window_ms)
        {
            if (max_count == 0 || ip.empty()) {
                return true;
            }

            const uint32_t effective_window = window_ms == 0 ? DEFAULT_RATE_LIMIT_WINDOW_MS : window_ms;
            const uint32_t now = base::time::now();
            std::lock_guard<std::mutex> lock(rate_mutex_);
            auto &state = states[ip];
            if (state.window_start_ms == 0 || now < state.window_start_ms || now - state.window_start_ms >= effective_window) {
                state.window_start_ms = now;
                state.count = 0;
            }

            if (state.count >= max_count) {
                return false;
            }

            ++state.count;
            return true;
        }
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
        if (!data_->config_.init(true)) {
            return false;
        }

#if defined(WS_USE_SSL)
        data_->ssl_module_ = std::make_shared<OpenSSLModule>();
        if (!data_->ssl_module_->init(data_->config_.get_tls_cert_path(), data_->config_.get_tls_key_path(), SSLHandler::SSLMode::acceptor_)) {
            if (auto msg = data_->ssl_module_->get_error_message()) {
                LOG_ERROR("{}", msg->c_str());
            }
            return false;
        }
        data_->listener_.set_ssl_module(data_->ssl_module_);
#endif

        data_->owned_runtime_ = std::make_unique<NetworkRuntime>();
        net::ListenOptions options;
        options.max_connections_per_ip = data_->config_.get_max_connections_per_ip();
        if (!data_->listener_.bind(port, *data_->owned_runtime_, options)) {
            data_->owned_runtime_.reset();
            return false;
        }

        return true;
    }

    bool WebSocketServer::init(int port, NetworkRuntime & runtime)
    {
        if (!data_->config_.init(true)) {
            return false;
        }

#if defined(WS_USE_SSL)
        data_->ssl_module_ = std::make_shared<OpenSSLModule>();
        if (!data_->ssl_module_->init(data_->config_.get_tls_cert_path(), data_->config_.get_tls_key_path(), SSLHandler::SSLMode::acceptor_)) {
            if (auto msg = data_->ssl_module_->get_error_message()) {
                LOG_ERROR("{}", msg->c_str());
            }
            return false;
        }
        data_->listener_.set_ssl_module(data_->ssl_module_);
#endif

        net::ListenOptions options;
        options.max_connections_per_ip = data_->config_.get_max_connections_per_ip();
        if (!data_->listener_.bind(port, runtime, options)) {
            return false;
        }

        return true;
    }

    void WebSocketServer::serve()
    {
        data_->listener_.set_connection_handler(
            [this](net::AsyncConnectionContext ctx)->coroutine::Task<void> {
                co_await handle_connection(std::move(ctx));
            });

        auto accept_task = data_->listener_.run_async();
        accept_task.resume();
        accept_task.detach();

        if (data_->owned_runtime_) {
            data_->owned_runtime_->run();
        }
    }

    void WebSocketServer::set_data_handler(WebSocketDataHandler * handler)
    {
        data_->data_handler_ = handler;
    }

    void WebSocketServer::set_origin_validator(std::function<bool(std::string_view)> validator)
    {
        data_->config_.set_origin_validator(std::move(validator));
    }

    void WebSocketServer::set_auth_validator(std::function<bool(const yuan::net::http::HttpRequest &)> validator)
    {
        data_->config_.set_auth_validator(std::move(validator));
    }

    void WebSocketServer::stop()
    {
        data_->listener_.close();
        std::unordered_set<std::shared_ptr<WebSocketConnection>> sessions;
        {
            std::lock_guard<std::mutex> lock(data_->sessions_mutex_);
            sessions.swap(data_->sessions_);
        }
        for (auto &session : sessions) {
            if (session) {
                session->shutdown();
            }
        }
        if (data_->owned_runtime_) {
            data_->owned_runtime_->stop();
        }
    }

    coroutine::Task<void> WebSocketServer::handle_connection(net::AsyncConnectionContext ctx)
    {
        auto conn = ctx.connection();
        const std::string client_ip = conn ? conn->get_remote_address().get_ip() : std::string();

        if (!data_->allow_rate(data_->handshake_rate_,
                               client_ip,
                               data_->config_.get_handshake_rate_limit_max(),
                               data_->config_.get_handshake_rate_limit_window())) {
            ctx.close();
            co_return;
        }

        if (conn && conn->is_ssl_handshaking()) {
            auto hs_result = co_await ctx.ssl_handshake_async();
            if (hs_result != coroutine::SslHandshakeResult::success) {
                co_return;
            }
        }

        auto wsConn = std::make_shared<WebSocketConnection>(WebSocketConnection::WorkMode::server_);
        wsConn->bind_connection(conn);
        wsConn->set_config(&data_->config_);
        {
            std::lock_guard<std::mutex> lock(data_->sessions_mutex_);
            data_->sessions_.insert(wsConn);
        }
        auto session_guard = std::shared_ptr<void>(wsConn.get(), [this, wsConn](void *) {
            std::lock_guard<std::mutex> lock(data_->sessions_mutex_);
            data_->sessions_.erase(wsConn);
        });

        if (data_->data_handler_) {
            wsConn->on_data = [this](WebSocketConnection *c, const ::yuan::buffer::ByteBuffer &buf) {
                data_->data_handler_->on_data(c, buf);
            };
            wsConn->on_connected_cb = [this](WebSocketConnection *c) {
                c->try_set_heartbeat_timer(data_->listener_.runtime());
                data_->data_handler_->on_connected(c);
            };
            wsConn->on_close_cb = [this](WebSocketConnection *c) {
                data_->data_handler_->on_close(c);
            };
        }

        {
            http::HttpSessionContext httpCtx(conn);
            httpCtx.set_mode(http::Mode::server);
            const uint32_t handshake_timeout = data_->config_.get_handshake_timeout();

            while (!wsConn->handshaker().is_handshake_done()) {
                auto read_result = co_await ctx.read_awaiter(handshake_timeout);
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
                    wsConn->handshaker().on_handshake(
                        httpCtx.get_request(), httpCtx.get_response(),
                        WebSocketConnection::WorkMode::server_);
                    if (!wsConn->handshaker().is_handshake_done()) {
                        httpCtx.process_error(http::ResponseCode::forbidden);
                        co_return;
                    }
                    wsConn->set_url(httpCtx.get_request()->get_raw_url());
                    wsConn->set_state(WebSocketConnection::State::connected_);
                    if (wsConn->on_connected_cb) {
                        wsConn->on_connected_cb(wsConn.get());
                    }
                }
            }

            auto leftover = httpCtx.take_leftover_buffer();
            if (!leftover.empty()) {
                if (!data_->allow_rate(data_->message_rate_,
                                       client_ip,
                                       data_->config_.get_message_rate_limit_max(),
                                       data_->config_.get_message_rate_limit_window())) {
                    wsConn->send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::policy_violation_);
                    ctx.close();
                    co_return;
                }
                if (!wsConn->pkt_parser().unpack_from(wsConn.get(), leftover)) {
                    wsConn->send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::invalid_palyload_);
                    ctx.close();
                    wsConn->set_state(WebSocketConnection::State::closed_);
                    if (wsConn->on_close_cb) {
                        wsConn->on_close_cb(wsConn.get());
                    }
                    co_return;
                }
            }
        }

        if (!wsConn->input_chunks().empty()) {
            auto result = wsConn->dispatch_frames(conn);
            if (result == FrameDispatchResult::close_) {
                wsConn->send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::normal_close_);
                ctx.close();
            } else if (result == FrameDispatchResult::error_) {
                wsConn->send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::invalid_palyload_);
                ctx.close();
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
            auto read_result = co_await ctx.read_awaiter(read_idle_timeout);
            if (read_result.status != coroutine::IoStatus::success) {
                break;
            }

            if (!data_->allow_rate(data_->message_rate_,
                                   client_ip,
                                   data_->config_.get_message_rate_limit_max(),
                                   data_->config_.get_message_rate_limit_window())) {
                wsConn->send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::policy_violation_);
                ctx.close();
                break;
            }

            if (!wsConn->pkt_parser().unpack_from(wsConn.get(), read_result.data)) {
                wsConn->send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::invalid_palyload_);
                ctx.close();
                break;
            }

            auto result = wsConn->dispatch_frames(conn);
            if (result == FrameDispatchResult::close_) {
                wsConn->send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::normal_close_);
                ctx.close();
                break;
            } else if (result == FrameDispatchResult::error_) {
                wsConn->send_close_frame_to(conn, (uint16_t)WebSocketCloseCode::invalid_palyload_);
                ctx.close();
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
