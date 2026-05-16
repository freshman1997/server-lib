#include "ssh_service.h"

namespace yuan::server
{

    SshService::SshService(int port, yuan::net::ssh::SshServerConfig config)
        : port_(port), config_(std::move(config)), server_(std::make_unique<yuan::net::ssh::SshServer>(config_)), host_({ "ssh", "ssh", port })
    {
    }

    SshService::~SshService()
    {
        stop();
    }

    bool SshService::init()
    {
        if (!server_) {
            return false;
        }

        if (handler_) {
            server_->set_handler(handler_);
        }

        if (shared_runtime_) {
            return server_->init(port_, *shared_runtime_);
        }
        return server_->init(port_);
    }

    void SshService::set_runtime_context(const yuan::app::RuntimeContext & context)
    {
        host_.set_runtime_context(context);
        shared_runtime_ = context.shared_runtime;
    }

    void SshService::start()
    {
        if (shared_runtime_) {
            host_.start_inline([this]() { server_->serve(); });
            return;
        }
        host_.start([this]() { server_->serve(); });
    }

    void SshService::stop()
    {
        host_.stop([this]() { server_->stop(); });
    }

    yuan::net::ssh::SshServer &SshService::server()
    {
        return *server_;
    }

    const yuan::net::ssh::SshServer &SshService::server() const
    {
        return *server_;
    }

    void SshService::set_handler(yuan::net::ssh::SshHandler * handler)
    {
        handler_ = handler;
        if (server_) {
            server_->set_handler(handler);
        }
    }
}
