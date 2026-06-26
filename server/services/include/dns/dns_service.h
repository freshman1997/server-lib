#ifndef __DNS_SERVICE_H__
#define __DNS_SERVICE_H__

#include "dns_server.h"
#include "server/proxy/include/server_runtime_host.h"
#include "service.h"

#include <memory>

namespace yuan::server
{

    class DnsService : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        explicit DnsService(int port);
        ~DnsService() override;

        bool init() override;
        void start() override;
        void stop() override;
        void set_runtime_context(const yuan::app::RuntimeContext &context) override;
        yuan::timer::TimerManager *resource_usage_timer_manager() override;

        yuan::net::dns::DnsServer &server();
        const yuan::net::dns::DnsServer &server() const;

    private:
        int port_;
        std::unique_ptr<yuan::net::dns::DnsServer> server_;
        ServerRuntimeHost host_;
        yuan::net::NetworkRuntime *shared_runtime_ = nullptr;
    };

} // namespace yuan::server

#endif
