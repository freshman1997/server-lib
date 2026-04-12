#include "websocket_service.h"
#include "eventbus/event_bus.h"
#include "server_service_events.h"

namespace
{

yuan::server::ServiceRuntimeEvent make_event(const yuan::app::RuntimeContext &context, int port)
{
    yuan::server::ServiceRuntimeEvent event;
    event.app_name = context.app_name;
    event.run_mode = context.run_mode;
    event.worker_threads = context.worker_threads;
    event.worker_index = context.worker_index;
    event.is_worker_process = context.is_worker_process;
    event.service_name = "websocket";
    event.protocol = "websocket";
    event.port = port;
    return event;
}

}

namespace yuan::server
{

WebSocketService::WebSocketService(int port)
    : port_(port)
    , server_(std::make_unique<yuan::net::websocket::WebSocketServer>())
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
    return server_->init(port_);
}

void WebSocketService::set_runtime_context(const yuan::app::RuntimeContext &context)
{
    runtime_context_ = context;
}

void WebSocketService::start()
{
    if (started_.exchange(true) || !server_) {
        return;
    }

    if (runtime_context_.event_bus) {
        runtime_context_.event_bus->publish(events::service_activating, make_event(runtime_context_, port_));
    }

    worker_ = std::thread([this]() {
        server_->serve();
        started_.store(false);
    });

    if (runtime_context_.event_bus) {
        runtime_context_.event_bus->publish(events::service_activated, make_event(runtime_context_, port_));
    }
}

void WebSocketService::stop()
{
    if (runtime_context_.event_bus) {
        runtime_context_.event_bus->publish(events::service_stopping, make_event(runtime_context_, port_));
    }

    if (server_) {
        server_->stop();
    }

    started_.store(false);
    if (worker_.joinable()) {
        worker_.join();
    }

    if (runtime_context_.event_bus) {
        runtime_context_.event_bus->publish(events::service_stopped, make_event(runtime_context_, port_));
    }
}

yuan::net::websocket::WebSocketServer& WebSocketService::server()
{
    return *server_;
}

const yuan::net::websocket::WebSocketServer& WebSocketService::server() const
{
    return *server_;
}

void WebSocketService::set_data_handler(yuan::net::websocket::WebSocketDataHandler* handler)
{
    handler_ = handler;
    if (server_) {
        server_->set_data_handler(handler);
    }
}

} // namespace yuan::server
