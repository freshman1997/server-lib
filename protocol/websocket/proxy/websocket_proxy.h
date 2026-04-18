#ifndef __NET_WEBSOCKET_PROXY_WEBSOCKET_PROXY_H__
#define __NET_WEBSOCKET_PROXY_WEBSOCKET_PROXY_H__

#include "../common/websocket_connection.h"
#include "../common/websocket_config.h"
#include "net/async/async_connection_context.h"
#include "net/async/async_client_session.h"
#include "coroutine/task.h"

#include <atomic>
#include <memory>
#include <string>

namespace yuan::net::http
{
    class HttpProxy;
    class HttpServer;
}

namespace yuan::net::websocket
{
    struct ProxySharedState;

    struct HandshakeBackendResult
    {
        bool success = false;
        std::string backend_subproto;
        ::yuan::buffer::ByteBuffer leftover_data;
    };

    class WebSocketProxy
    {
    public:
        using Ptr = std::shared_ptr<WebSocketProxy>;

        WebSocketProxy(http::HttpProxy *http_proxy, http::HttpServer *server);
        ~WebSocketProxy() = default;

        WebSocketProxy(const WebSocketProxy &) = delete;
        WebSocketProxy &operator=(const WebSocketProxy &) = delete;

        coroutine::Task<void> proxy_connection(
            net::AsyncConnectionContext client_ctx,
            const std::string &raw_url,
            const std::string &route_key,
            const std::string &client_key,
            const std::string &subproto,
            ::yuan::buffer::ByteBuffer client_leftover);

        static void install(http::HttpServer &server, http::HttpProxy &proxy);

    private:
        coroutine::Task<HandshakeBackendResult> handshake_backend(
            net::AsyncClientSession &session,
            const std::string &host,
            uint16_t port,
            const std::string &path,
            int connect_timeout_ms,
            const std::string &client_ip,
            const std::string &subproto);

        coroutine::Task<void> forward_frames(
            std::shared_ptr<ProxySharedState> state,
            bool client_to_backend,
            ::yuan::buffer::ByteBuffer initial_data = {});

        static coroutine::Task<void> send_control_frame_async(
            net::AsyncConnectionContext &ctx,
            WebSocketConnection &ws,
            uint8_t opcode,
            const ::yuan::buffer::ByteBuffer &payload);

        http::HttpProxy *http_proxy_;
        http::HttpServer *server_;
        WebSocketConfigManager server_config_;
        WebSocketConfigManager client_config_;
    };

    struct ProxySharedState
    {
        net::AsyncConnectionContext client_ctx;
        net::AsyncConnectionContext backend_ctx;
        WebSocketConnection client_ws;
        WebSocketConnection backend_ws;

        std::atomic<bool> closed{ false };
        std::atomic<int> active_count{ 2 };

        ProxySharedState(net::AsyncConnectionContext &&cctx,
                         net::AsyncConnectionContext &&bctx,
                         WebSocketConfigManager *server_cfg,
                         WebSocketConfigManager *client_cfg)
            : client_ctx(std::move(cctx)), backend_ctx(std::move(bctx)), client_ws(WebSocketConnection::WorkMode::server_), backend_ws(WebSocketConnection::WorkMode::client_)
        {
            client_ws.bind_connection(client_ctx.connection());
            client_ws.set_config(server_cfg);
            client_ws.set_state(WebSocketConnection::State::connected_);

            backend_ws.bind_connection(backend_ctx.connection());
            backend_ws.set_config(client_cfg);
            backend_ws.set_state(WebSocketConnection::State::connected_);
        }

        void close_all()
        {
            client_ws.set_state(WebSocketConnection::State::closed_);
            backend_ws.set_state(WebSocketConnection::State::closed_);
            client_ctx.close();
            backend_ctx.close();
        }
    };
}
#endif
