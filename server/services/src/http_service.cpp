#include "http_service.h"
#include "request.h"
#include "response.h"
#include "service_registry.h"
#include "proxy.h"

#if __has_include("proxy/websocket_proxy.h")
#include "proxy/websocket_proxy.h"
#define YUAN_HAS_WEBSOCKET_PROXY 1
#endif

namespace yuan::server
{

    HttpService::HttpService(int port, yuan::net::http::HttpServerConfig config)
        : port_(port), config_(std::move(config)), server_(std::make_unique<yuan::net::http::HttpServer>(config_)), host_({ "http", "http", port })
    {
    }

    HttpService::~HttpService()
    {
        stop();
    }

    bool HttpService::init()
    {
        if (!server_) {
            return false;
        }

        bool ok = false;
        if (shared_runtime_) {
            ok = server_->init(port_, *shared_runtime_);
        } else {
            ok = server_->init(port_);
        }

        if (ok) {
#ifdef YUAN_HAS_WEBSOCKET_PROXY
            auto *proxy = server_->get_proxy();
            if (proxy) {
                yuan::net::websocket::WebSocketProxy::install(*server_, *proxy);
            }
#endif
        }

        return ok;
    }

    void HttpService::set_runtime_context(const yuan::app::RuntimeContext & context)
    {
        host_.set_runtime_context(context);
        shared_runtime_ = context.shared_runtime;
    }

    void HttpService::start()
    {
        host_.start([this]() { server_->serve(); });
    }

    void HttpService::stop()
    {
        host_.stop([this]() { server_->stop(); });
    }

    yuan::net::http::HttpServer &HttpService::server()
    {
        return *server_;
    }

    const yuan::net::http::HttpServer &HttpService::server() const
    {
        return *server_;
    }

} // namespace yuan::server
