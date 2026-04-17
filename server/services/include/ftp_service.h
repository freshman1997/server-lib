#ifndef __SERVER_FTP_SERVICE_H__
#define __SERVER_FTP_SERVICE_H__

#include "server/ftp_server.h"
#include "server_runtime_host.h"
#include "service.h"

#include <memory>

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

        yuan::net::ftp::FtpServer &server();
        const yuan::net::ftp::FtpServer &server() const;

    private:
        int port_;
        std::unique_ptr<yuan::net::ftp::FtpServer> server_;
        ServerRuntimeHost host_;
        yuan::net::NetworkRuntime *shared_runtime_ = nullptr;
    };

} // namespace yuan::server

#endif
