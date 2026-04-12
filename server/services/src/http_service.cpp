#include "http_service.h"
#include "eventbus/event_bus.h"
#include "request.h"
#include "response.h"
#include "server_service_events.h"
#include "service_registry.h"

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
    event.service_name = "http";
    event.protocol = "http";
    event.port = port;
    return event;
}

}

namespace yuan::server
{

HttpService::HttpService(int port, yuan::net::http::HttpServerConfig config)
    : port_(port)
    , config_(std::move(config))
    , server_(std::make_unique<yuan::net::http::HttpServer>(config_))
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

    return server_->init(port_);
}

void HttpService::set_runtime_context(const yuan::app::RuntimeContext &context)
{
    runtime_context_ = context;
}

void HttpService::start()
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

void HttpService::stop()
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

yuan::net::http::HttpServer& HttpService::server()
{
    return *server_;
}

const yuan::net::http::HttpServer& HttpService::server() const
{
    return *server_;
}

} // namespace yuan::server
