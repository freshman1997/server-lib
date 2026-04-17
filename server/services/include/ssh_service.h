#ifndef __SERVER_SSH_SERVICE_H__
#define __SERVER_SSH_SERVICE_H__

#include "ssh.h"
#include "server_runtime_host.h"
#include "service.h"

#include <memory>

namespace yuan::server
{

    class SshService : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        explicit SshService(int port, yuan::net::ssh::SshServerConfig config = {});
        ~SshService() override;

        bool init() override;
        void start() override;
        void stop() override;
        void set_runtime_context(const yuan::app::RuntimeContext &context) override;

        yuan::net::ssh::SshServer &server();
        const yuan::net::ssh::SshServer &server() const;

        void set_handler(yuan::net::ssh::SshHandler *handler);

    private:
        int port_;
        yuan::net::ssh::SshServerConfig config_;
        std::unique_ptr<yuan::net::ssh::SshServer> server_;
        yuan::net::ssh::SshHandler *handler_ = nullptr;
        ServerRuntimeHost host_;
        yuan::net::NetworkRuntime *shared_runtime_ = nullptr;
    };
}

#endif
