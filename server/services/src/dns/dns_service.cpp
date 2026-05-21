#include "dns/dns_service.h"

namespace yuan::server
{

    DnsService::DnsService(int port)
        : port_(port), server_(std::make_unique<yuan::net::dns::DnsServer>()), host_({ "dns", "dns", port })
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

    void DnsService::set_runtime_context(const yuan::app::RuntimeContext & context)
    {
        host_.set_runtime_context(context);
        shared_runtime_ = context.shared_runtime;
    }

    void DnsService::start()
    {
        if (shared_runtime_) {
            host_.start_inline([this]() { server_->serve(port_, *shared_runtime_); });
        } else {
            host_.start([this]() { server_->serve(port_); });
        }
    }

    void DnsService::stop()
    {
        host_.stop([this]() { server_->stop(); });
    }

    yuan::net::dns::DnsServer &DnsService::server()
    {
        return *server_;
    }

    const yuan::net::dns::DnsServer &DnsService::server() const
    {
        return *server_;
    }

} // namespace yuan::server
