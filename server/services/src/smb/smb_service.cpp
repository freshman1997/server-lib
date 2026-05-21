#include "smb/smb_service.h"

namespace yuan::server
{

    SmbService::SmbService(int port, yuan::net::smb::SmbServerConfig config)
        : port_(port), config_(std::move(config)), server_(std::make_unique<yuan::net::smb::SmbServer>(config_)), host_({ "smb", "smb", port })
    {
    }

    SmbService::~SmbService()
    {
        stop();
    }

    bool SmbService::init()
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

    void SmbService::set_runtime_context(const yuan::app::RuntimeContext & context)
    {
        host_.set_runtime_context(context);
        shared_runtime_ = context.shared_runtime;
    }

    void SmbService::start()
    {
        if (shared_runtime_) {
            host_.start_inline([this]() { server_->serve(); });
            return;
        }
        host_.start([this]() { server_->serve(); });
    }

    void SmbService::stop()
    {
        auto *server = server_.get();
        host_.stop([server]() {
            if (server) {
                server->stop();
            }
        });
    }

    yuan::net::smb::SmbServer &SmbService::server()
    {
        return *server_;
    }

    const yuan::net::smb::SmbServer &SmbService::server() const
    {
        return *server_;
    }

    void SmbService::set_handler(yuan::net::smb::SmbHandler * handler)
    {
        handler_ = handler;
        if (server_) {
            server_->set_handler(handler);
        }
    }
}
