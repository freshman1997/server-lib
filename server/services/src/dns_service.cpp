#include "dns_service.h"
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
    event.service_name = "dns";
    event.protocol = "dns";
    event.port = port;
    return event;
}

}

namespace yuan::server
{

DnsService::DnsService(int port)
    : port_(port)
    , server_(std::make_unique<yuan::net::dns::DnsServer>())
{
}

DnsService::~DnsService()
{
    stop();
}

bool DnsService::init()
{
    if (!server_ || port_ <= 0) {
        return false;
    }

    server_->add_record("webserver.local", "127.0.0.1");
    return true;
}

void DnsService::set_runtime_context(const yuan::app::RuntimeContext &context)
{
    runtime_context_ = context;
}

void DnsService::start()
{
    if (started_.exchange(true) || !server_) {
        return;
    }

    if (runtime_context_.event_bus) {
        runtime_context_.event_bus->publish(events::service_activating, make_event(runtime_context_, port_));
    }

    worker_ = std::thread([this]() {
        server_->serve(port_);
        started_.store(false);
    });

    if (runtime_context_.event_bus) {
        runtime_context_.event_bus->publish(events::service_activated, make_event(runtime_context_, port_));
    }
}

void DnsService::stop()
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

yuan::net::dns::DnsServer& DnsService::server()
{
    return *server_;
}

const yuan::net::dns::DnsServer& DnsService::server() const
{
    return *server_;
}

} // namespace yuan::server
