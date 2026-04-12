#ifndef __DNS_SERVICE_H__
#define __DNS_SERVICE_H__

#include "dns_server.h"
#include "service.h"

#include <atomic>
#include <memory>
#include <thread>

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

    yuan::net::dns::DnsServer& server();
    const yuan::net::dns::DnsServer& server() const;

private:
    int port_;
    yuan::app::RuntimeContext runtime_context_{};
    std::atomic<bool> started_{false};
    std::unique_ptr<yuan::net::dns::DnsServer> server_;
    std::thread worker_;
};

} // namespace yuan::server

#endif
