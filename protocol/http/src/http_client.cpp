#include "logger.h"
#include "coroutine/runtime.h"
#include "coroutine/sync_wait.h"
#include "coroutine/stream_io_awaitable.h"
#include "net/runtime/network_runtime.h"
#include "net/connection/stream_transport.h"
#include "context.h"
#include "http_client.h"
#include "ops/option.h"
#include "response.h"
#include "session.h"
#include "net/secuity/openssl.h"

namespace yuan::net::http
{
    HttpClient::HttpClient()
        : port_(80), ssl_module_(nullptr)
    {
    }

    HttpClient::~HttpClient()
    {
        delete last_session_;
        session_.close();
    }

    bool HttpClient::query(const std::string & url)
    {
        if (url.find("://") == std::string::npos) {
            return false;
        }
        size_t pos = url.find("://");
        std::string protocol = url.substr(0, pos);
        bool is_https = (protocol == "https");
        if (protocol != "http" && protocol != "https") {
            return false;
        }

        std::string rest = url.substr(pos + 3);
        size_t port_pos = rest.find(":");
        size_t path_pos = rest.find("/");

        if (port_pos != std::string::npos) {
            size_t port_end = (path_pos != std::string::npos && path_pos > port_pos) ? path_pos : rest.size();
            std::string port_str = rest.substr(port_pos + 1, port_end - port_pos - 1);
            port_ = std::stoi(port_str);
            if (port_ <= 0 || port_ > 65535) {
                return false;
            }
        } else {
            port_ = is_https ? 443 : 80;
            port_pos = std::string::npos;
        }

        if (path_pos == std::string::npos) {
            path_pos = rest.size();
        }

        host_name_ = rest.substr(0, (port_pos != std::string::npos && port_pos < path_pos) ? port_pos : path_pos);

        return true;
    }

    bool HttpClient::connect(connected_callback ccb, request_function rcb)
    {
        if (!ccb || !rcb) {
            LOG_ERROR("must set callback!");
            return false;
        }

        owned_runtime_ = std::make_unique<net::NetworkRuntime>();
        auto rv = owned_runtime_->runtime_view();

        auto *response = yuan::coroutine::sync_wait(
            rv,
            do_connect_async(rv, std::move(ccb), config::connection_idle_timeout));

        if (response && rcb) {
            auto *context = response->get_context();
            rcb(context->get_request(), response);
        }

        return response != nullptr;
    }

    yuan::coroutine::Task<HttpResponse *> HttpClient::do_connect_async(
        yuan::coroutine::RuntimeView runtime,
        connected_callback ccb,
        uint32_t timeout_ms)
    {
        if (!ccb) {
            LOG_ERROR("must set connected callback!");
            co_return nullptr;
        }

        auto *loop = runtime.event_loop();
        if (!loop) {
            LOG_ERROR("runtime must provide event loop");
            co_return nullptr;
        }

        bool ok = co_await session_.connect_async(runtime, host_name_, port_, timeout_ms);
        if (!ok) {
            co_return nullptr;
        }

        Connection *conn = session_.native_handle();
        if (!conn) {
            co_return nullptr;
        }

        bool is_https = (port_ == 443);
        if (is_https) {
            if (!ssl_module_) {
                ssl_module_ = std::make_shared<OpenSSLModule>();
                if (!ssl_module_->init("./ca/ca.crt")) {
                    ssl_module_.reset();
                    co_return nullptr;
                }
            }

            auto *stream = dynamic_cast<StreamTransport *>(conn);
            auto *channel = stream ? stream->stream_channel() : nullptr;
            if (!channel) {
                co_return nullptr;
            }

            auto sslHandler = ssl_module_->create_handler(channel->get_fd(), SSLHandler::SSLMode::connector_);
            if (!sslHandler) {
                co_return nullptr;
            }

            conn->set_ssl_handler(sslHandler);

            auto handshake_result = co_await coroutine::async_ssl_handshake(runtime, conn, timeout_ms);
            if (handshake_result != coroutine::SslHandshakeResult::success) {
                co_return nullptr;
            }
        }

        HttpSessionContext *httpCtx = new HttpSessionContext(conn);
        httpCtx->set_mode(Mode::client);

        delete last_session_;
        last_session_ = nullptr;
        HttpSession *httpSession = new HttpSession(reinterpret_cast<uintptr_t>(conn), httpCtx, runtime);

        ccb(httpCtx->get_request());

        const uint32_t read_timeout = timeout_ms > 0 ? timeout_ms : config::connection_idle_timeout;

        while (true) {
            auto read_result = co_await session_.read_async(read_timeout);
            if (read_result.status != coroutine::IoStatus::success) {
                break;
            }

            if (!httpCtx->parse_from(read_result.data)) {
                if (httpCtx->has_error()) {
                    break;
                }
                continue;
            }

            if (httpCtx->has_error()) {
                break;
            }

            if (httpCtx->is_downloading()) {
                continue;
            }

            if (httpCtx->is_completed()) {
                (void)httpCtx->try_parse_request_content();

                if (httpCtx->is_completed()) {
                    last_session_ = httpSession;
                    httpSession = nullptr;
                    co_return last_session_->get_context()->get_response();
                }
            }

            httpSession->reset_timer();
        }

        delete httpSession;
        co_return nullptr;
    }

    yuan::coroutine::Task<HttpResponse *> HttpClient::connect_async(
        net::NetworkRuntime::RuntimeView runtime,
        connected_callback ccb,
        uint32_t timeout_ms)
    {
        co_return co_await do_connect_async(runtime, std::move(ccb), timeout_ms);
    }

    yuan::coroutine::Task<HttpResponse *> HttpClient::connect_async(
        connected_callback ccb,
        uint32_t timeout_ms)
    {
        if (!ccb) {
            LOG_ERROR("must set connected callback!");
            co_return nullptr;
        }

        net::NetworkRuntime runtime;
        auto rv = runtime.runtime_view();

        co_return yuan::coroutine::sync_wait(
            rv,
            do_connect_async(rv, std::move(ccb), timeout_ms));
    }

    HttpResponseSnapshot HttpClient::snapshot_response(HttpResponse * response)
    {
        HttpResponseSnapshot snapshot;
        if (!response) {
            return snapshot;
        }

        snapshot.good = response->good();
        snapshot.response_code = response->get_response_code();
        snapshot.content_type = response->get_content_type();
        snapshot.downloading = response->is_downloading();
        snapshot.original_file_name = response->get_original_file_name();
        snapshot.headers = response->headers();
        if (const char *begin = response->body_begin()) {
            snapshot.body.assign(begin, response->get_body_length());
        }

        return snapshot;
    }

    yuan::coroutine::Task<HttpResponseSnapshot> HttpClient::connect_snapshot_async(
        net::NetworkRuntime::RuntimeView runtime,
        connected_callback ccb,
        uint32_t timeout_ms)
    {
        co_return snapshot_response(co_await connect_async(runtime, std::move(ccb), timeout_ms));
    }

    yuan::coroutine::Task<HttpResponseSnapshot> HttpClient::connect_snapshot_async(
        connected_callback ccb,
        uint32_t timeout_ms)
    {
        co_return snapshot_response(co_await connect_async(std::move(ccb), timeout_ms));
    }
}
