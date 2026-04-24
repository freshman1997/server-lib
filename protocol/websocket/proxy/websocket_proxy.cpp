#include "websocket_proxy.h"
#include "../common/websocket_utils.h"
#include "../common/close_code.h"
#include "proxy.h"
#include "http_server.h"
#include "context.h"
#include "request.h"
#include "response.h"
#include "response_code.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/inet_address.h"
#include "coroutine/io_result.h"
#include "logger.h"

#include <algorithm>
#include <cstring>

namespace yuan::net::websocket
{
    void WebSocketProxy::install(http::HttpServer & server, http::HttpProxyHandler & proxy)
    {
        auto ws_proxy = std::make_shared<WebSocketProxy>(&proxy, &server);
        server.set_ws_proxy_handler(
            [ws_proxy](net::AsyncConnectionContext ctx, const std::string & url,
                       const std::string & route_key, const std::string & client_key,
                       const std::string & subproto, ::yuan::buffer::ByteBuffer leftover)->coroutine::Task<void> {
                co_await ws_proxy->proxy_connection(std::move(ctx), url, route_key, client_key, subproto, std::move(leftover)); });
    }

    WebSocketProxy::WebSocketProxy(http::HttpProxyHandler * http_proxy, http::HttpServer * server)
        : http_proxy_(http_proxy), server_(server)
    {
        server_config_.init(true);
        client_config_.init(false);
        client_config_.force_client_mask();
    }

    coroutine::Task<void> WebSocketProxy::proxy_connection(
        net::AsyncConnectionContext client_ctx,
        const std::string & raw_url,
        const std::string & route_key,
        const std::string & client_key,
        const std::string & subproto,
        ::yuan::buffer::ByteBuffer client_leftover)
    {
        if (route_key.empty()) {
            client_ctx.close();
            co_return;
        }

        const auto &routes = http_proxy_->get_routes();
        auto routeIt = routes.find(route_key);
        if (routeIt == routes.end()) {
            client_ctx.close();
            co_return;
        }

        const http::ProxyRoute &route = routeIt->second;
        http::ProxyTarget target = http_proxy_->select_target_public(route);

        auto *runtime = server_->runtime();
        if (!runtime) {
            client_ctx.close();
            co_return;
        }

        auto rv = runtime->runtime_view();

        net::AsyncClientSession backend_session;
        bool connected = co_await backend_session.connect_async(
            rv, target.host, target.port,
            static_cast<uint32_t>(route.connect_timeout_ms));
        if (!connected) {
            LOG_WARN("[WSProxy] connect to {}:{} failed", target.host, target.port);
            client_ctx.close();
            co_return;
        }

        std::string path = raw_url.empty() ? "/" : raw_url;

        std::string client_ip;
        if (client_ctx.is_connected()) {
            client_ip = client_ctx.get_remote_address().to_address_key();
        }

        auto hs_result = co_await handshake_backend(
            backend_session,
            target.host, target.port,
            path, route.connect_timeout_ms,
            client_ip, subproto);
        if (!hs_result.success) {
            LOG_WARN("[WSProxy] backend handshake failed for {}:{}", target.host, target.port);
            client_ctx.close();
            backend_session.close();
            co_return;
        }

        std::string server_accept = WebSocketUtils::generate_server_key(client_key);

        std::string resp_str = "HTTP/1.1 101 Switching Protocols\r\n"
                               "Connection: Upgrade\r\n"
                               "Upgrade: websocket\r\n"
                               "Sec-WebSocket-Accept: " +
                               server_accept + "\r\n";

        std::string agreed_subproto = hs_result.backend_subproto.empty()
                                          ? subproto
                                          : hs_result.backend_subproto;
        if (!agreed_subproto.empty()) {
            resp_str += "Sec-WebSocket-Protocol: " + agreed_subproto + "\r\n";
        }
        resp_str += "\r\n";

        ::yuan::buffer::ByteBuffer resp_buf(resp_str.size());
        resp_buf.append(std::string_view(resp_str));
        auto write_result = co_await client_ctx.write_async(resp_buf);
        if (write_result.status != coroutine::IoStatus::success) {
            client_ctx.close();
            backend_session.close();
            co_return;
        }
        co_await client_ctx.flush_async();

        LOG_INFO("[WSProxy] proxy established: client <-> {}:{}", target.host, target.port);

        auto state = std::make_shared<ProxySharedState>(
            std::move(client_ctx),
            std::move(backend_session.context()),
            &server_config_,
            &client_config_);

        auto b2c = forward_frames(state, false, std::move(hs_result.leftover_data));
        b2c.resume();
        b2c.detach();

        co_await forward_frames(state, true, std::move(client_leftover));

        co_return;
    }

    coroutine::Task<HandshakeBackendResult> WebSocketProxy::handshake_backend(
        net::AsyncClientSession & session,
        const std::string & host,
        uint16_t port,
        const std::string & path,
        int connect_timeout_ms,
        const std::string & client_ip,
        const std::string & subproto)
    {
        (void)connect_timeout_ms;

        HandshakeBackendResult result;

        std::string proxy_key = client_config_.get_client_key();

        std::string req_str = "GET " + path + " HTTP/1.1\r\n"
                                              "Host: " +
                              host + ":" + std::to_string(port) + "\r\n"
                                                                  "Upgrade: websocket\r\n"
                                                                  "Connection: Upgrade\r\n"
                                                                  "Sec-WebSocket-Key: " +
                              proxy_key + "\r\n"
                                          "Sec-WebSocket-Version: 13\r\n";

        if (!client_ip.empty()) {
            req_str += "X-Forwarded-For: " + client_ip + "\r\n";
            req_str += "X-Real-IP: " + client_ip + "\r\n";
        }

        if (!subproto.empty()) {
            req_str += "Sec-WebSocket-Protocol: " + subproto + "\r\n";
        }

        req_str += "X-Forwarded-Proto: http\r\n";
        req_str += "\r\n";

        ::yuan::buffer::ByteBuffer req_buf(req_str.size());
        req_buf.append(std::string_view(req_str));
        auto write_result = co_await session.write_async(req_buf);
        if (write_result.status != coroutine::IoStatus::success) {
            co_return result;
        }
        co_await session.flush_async();

        http::HttpSessionContext httpCtx(session.context().connection());
        httpCtx.set_mode(http::Mode::client);

        std::string expected_accept = WebSocketUtils::generate_server_key(proxy_key);
        bool handshake_ok = false;

        for (int i = 0; i < 64; ++i) {
            auto read_result = co_await session.read_async();
            if (read_result.status != coroutine::IoStatus::success) {
                break;
            }

            if (!httpCtx.parse_from(read_result.data)) {
                break;
            }

            if (httpCtx.has_error()) {
                break;
            }

            if (httpCtx.is_completed()) {
                auto *resp = httpCtx.get_response();
                if (!resp->is_ok()) {
                    break;
                }

                auto *conn_hdr = resp->get_header("connection");
                if (!conn_hdr)
                    break;
                std::string conn_lower = *conn_hdr;
                std::transform(conn_lower.begin(), conn_lower.end(), conn_lower.begin(), ::tolower);
                if (conn_lower != "upgrade")
                    break;

                auto *upgrade_hdr = resp->get_header("upgrade");
                if (!upgrade_hdr)
                    break;
                std::string upgrade_lower = *upgrade_hdr;
                std::transform(upgrade_lower.begin(), upgrade_lower.end(), upgrade_lower.begin(), ::tolower);
                if (upgrade_lower != "websocket")
                    break;

                auto *accept_hdr = resp->get_header("sec-websocket-accept");
                if (!accept_hdr || *accept_hdr != expected_accept) {
                    break;
                }

                auto *subproto_hdr = resp->get_header("sec-websocket-protocol");
                if (subproto_hdr && !subproto_hdr->empty()) {
                    result.backend_subproto = *subproto_hdr;
                }

                handshake_ok = true;
                break;
            }
        }

        if (handshake_ok) {
            result.leftover_data = httpCtx.take_leftover_buffer();
        }

        result.success = handshake_ok;
        co_return result;
    }

    coroutine::Task<void> WebSocketProxy::forward_frames(
        std::shared_ptr<ProxySharedState> state,
        bool client_to_backend,
        ::yuan::buffer::ByteBuffer initial_data)
    {
        net::AsyncConnectionContext &src_ctx = client_to_backend ? state->client_ctx : state->backend_ctx;
        net::AsyncConnectionContext &dst_ctx = client_to_backend ? state->backend_ctx : state->client_ctx;
        WebSocketConnection &src_ws = client_to_backend ? state->client_ws : state->backend_ws;
        WebSocketConnection &dst_ws = client_to_backend ? state->backend_ws : state->client_ws;

        bool has_initial = !initial_data.empty();

        while (!state->closed.load(std::memory_order_acquire) &&
               src_ctx.is_connected() && dst_ctx.is_connected()) {

            if (has_initial) {
                has_initial = false;
                if (!src_ws.pkt_parser().unpack_from(&src_ws, initial_data)) {
                    ::yuan::buffer::ByteBuffer empty_payload;
                    co_await send_control_frame_async(src_ctx, src_ws,
                                                      static_cast<uint8_t>(OpCodeType::type_close_frame), empty_payload);
                    break;
                }
                initial_data.clear();
            } else {
                auto read_result = co_await src_ctx.read_async();
                if (read_result.status != coroutine::IoStatus::success) {
                    break;
                }

                if (!src_ws.pkt_parser().unpack_from(&src_ws, read_result.data)) {
                    ::yuan::buffer::ByteBuffer empty_payload;
                    co_await send_control_frame_async(src_ctx, src_ws,
                                                      static_cast<uint8_t>(OpCodeType::type_close_frame), empty_payload);
                    break;
                }
            }

            auto &chunks = src_ws.input_chunks();
            bool should_close = false;

            for (auto &chunk : chunks) {
                if (!chunk.is_completed() || !chunk.head_.is_fin()) {
                    continue;
                }

                if (chunk.head_.is_close_frame()) {
                    ::yuan::buffer::ByteBuffer close_payload;
                    if (chunk.body_.readable_bytes() >= 2) {
                        close_payload.append(chunk.body_);
                    } else {
                        uint16_t code = static_cast<uint16_t>(WebSocketCloseCode::normal_close_);
                        close_payload.append_u8(static_cast<uint8_t>((code >> 8) & 0xff));
                        close_payload.append_u8(static_cast<uint8_t>(code & 0xff));
                    }
                    co_await send_control_frame_async(dst_ctx, dst_ws,
                                                      static_cast<uint8_t>(OpCodeType::type_close_frame), close_payload);
                    should_close = true;
                    break;
                } else if (chunk.head_.is_ping_frame()) {
                    co_await send_control_frame_async(src_ctx, src_ws,
                                                      static_cast<uint8_t>(OpCodeType::type_pong_frame), chunk.body_);
                } else if (chunk.head_.is_pong_frame()) {
                } else {
                    if (state->closed.load(std::memory_order_acquire)) {
                        should_close = true;
                        break;
                    }

                    uint8_t opcode = chunk.head_.ctrl_code_.opcode_;
                    std::vector< ::yuan::buffer::ByteBuffer> output;
                    if (dst_ws.pack_frame(chunk.body_, opcode, output)) {
                        for (auto &buf : output) {
                            auto wr = co_await dst_ctx.write_async(buf);
                            if (wr.status != coroutine::IoStatus::success) {
                                should_close = true;
                                break;
                            }
                        }
                        if (!should_close) {
                            auto fr = co_await dst_ctx.flush_async();
                            if (fr.status != coroutine::IoStatus::success) {
                                should_close = true;
                            }
                        }
                    }
                }
            }

            auto &mutable_chunks = src_ws.input_chunks();
            size_t write_idx = 0;
            for (size_t i = 0; i < mutable_chunks.size(); ++i) {
                if (!mutable_chunks[i].is_completed() || !mutable_chunks[i].head_.is_fin()) {
                    if (write_idx != i) {
                        mutable_chunks[write_idx] = std::move(mutable_chunks[i]);
                    }
                    ++write_idx;
                }
            }
            mutable_chunks.erase(mutable_chunks.begin() + write_idx, mutable_chunks.end());

            if (should_close) {
                break;
            }
        }

        state->closed.store(true, std::memory_order_release);

        state->client_ctx.close();
        state->backend_ctx.close();

        if (state->active_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            state->close_all();
        }
    }

    coroutine::Task<void> WebSocketProxy::send_control_frame_async(
        net::AsyncConnectionContext & ctx,
        WebSocketConnection & ws,
        uint8_t opcode,
        const ::yuan::buffer::ByteBuffer & payload)
    {
        std::vector< ::yuan::buffer::ByteBuffer> output;
        if (!ws.pack_control_frame(payload, opcode, output)) {
            co_return;
        }
        for (auto &buf : output) {
            auto wr = co_await ctx.write_async(buf);
            if (wr.status != coroutine::IoStatus::success) {
                break;
            }
        }
        co_await ctx.flush_async();
    }
}
