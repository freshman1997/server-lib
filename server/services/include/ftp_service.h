#ifndef __SERVER_FTP_SERVICE_H__
#define __SERVER_FTP_SERVICE_H__

#include "server/ftp_server.h"
#include "service.h"

#include <atomic>
#include <memory>
#include <thread>

namespace yuan::server
{

class FtpService : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
{
public:
    explicit FtpService(int port);
    ~FtpService() override;

    bool init() override;
    void start() override;
    void stop() override;
    void set_runtime_context(const yuan::app::RuntimeContext &context) override;

    yuan::net::ftp::FtpServer& server();
    const yuan::net::ftp::FtpServer& server() const;

private:
    int port_;
    yuan::app::RuntimeContext runtime_context_{};
    std::unique_ptr<yuan::net::ftp::FtpServer> server_;
    std::atomic<bool> started_{false};
    std::thread worker_;
};

} // namespace yuan::server

#endif
