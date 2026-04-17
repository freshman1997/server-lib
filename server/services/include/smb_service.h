#ifndef __SERVER_SMB_SERVICE_H__
#define __SERVER_SMB_SERVICE_H__

#include "smb.h"
#include "server_runtime_host.h"
#include "service.h"

#include <memory>

namespace yuan::server
{

    class SmbService : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        explicit SmbService(int port, yuan::net::smb::SmbServerConfig config = {});
        ~SmbService() override;

        bool init() override;
        void start() override;
        void stop() override;
        void set_runtime_context(const yuan::app::RuntimeContext &context) override;

        yuan::net::smb::SmbServer &server();
        const yuan::net::smb::SmbServer &server() const;

        void set_handler(yuan::net::smb::SmbHandler *handler);

    private:
        int port_;
        yuan::net::smb::SmbServerConfig config_;
        std::unique_ptr<yuan::net::smb::SmbServer> server_;
        yuan::net::smb::SmbHandler *handler_ = nullptr;
        ServerRuntimeHost host_;
        yuan::net::NetworkRuntime *shared_runtime_ = nullptr;
    };
}

#endif
