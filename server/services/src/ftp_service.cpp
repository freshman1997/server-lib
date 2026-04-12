#include "ftp_service.h"
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
    event.service_name = "ftp";
    event.protocol = "ftp";
    event.port = port;
    return event;
}

}

namespace yuan::server
{

FtpService::FtpService(int port)
    : port_(port)
    , server_(std::make_unique<yuan::net::ftp::FtpServer>())
{
}

FtpService::~FtpService()
{
    stop();
}

bool FtpService::init()
{
    return port_ > 0 && server_ != nullptr;
}

void FtpService::set_runtime_context(const yuan::app::RuntimeContext &context)
{
    runtime_context_ = context;
}

void FtpService::start()
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

void FtpService::stop()
{
    if (runtime_context_.event_bus) {
        runtime_context_.event_bus->publish(events::service_stopping, make_event(runtime_context_, port_));
    }

    if (server_) {
        server_->quit();
    }

    started_.store(false);
    if (worker_.joinable()) {
        worker_.join();
    }

    if (runtime_context_.event_bus) {
        runtime_context_.event_bus->publish(events::service_stopped, make_event(runtime_context_, port_));
    }
}

yuan::net::ftp::FtpServer& FtpService::server()
{
    return *server_;
}

const yuan::net::ftp::FtpServer& FtpService::server() const
{
    return *server_;
}

} // namespace yuan::server
