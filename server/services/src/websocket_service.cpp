#include "websocket_service.h"

namespace yuan::server
{

    WebSocketService::WebSocketService(int port)
        : port_(port), server_(std::make_unique<yuan::net::websocket::WebSocketServer>()), host_({ "websocket", "websocket", port })
    {
    }

    WebSocketService::~WebSocketService()
    {
        stop();
    }

    bool WebSocketService::init()
    {
        if (!server_) {
            return false;
        }

        if (handler_) {
            server_->set_data_handler(handler_);
        }

        if (shared_runtime_) {
            return server_->init(port_, *shared_runtime_);
        }
        return server_->init(port_);
    }

    void WebSocketService::set_runtime_context(const yuan::app::RuntimeContext & context)
    {
        host_.set_runtime_context(context);
        shared_runtime_ = context.shared_runtime;
    }

    void WebSocketService::start()
    {
        host_.start([this]() { server_->serve(); });
    }

    void WebSocketService::stop()
    {
        host_.stop([this]() { server_->stop(); });
    }

    yuan::net::websocket::WebSocketServer &WebSocketService::server()
    {
        return *server_;
    }

    const yuan::net::websocket::WebSocketServer &WebSocketService::server() const
    {
        return *server_;
    }

    void WebSocketService::set_data_handler(yuan::net::websocket::WebSocketDataHandler * handler)
    {
        handler_ = handler;
        if (server_) {
            server_->set_data_handler(handler);
        }
    }

} // namespace yuan::server
