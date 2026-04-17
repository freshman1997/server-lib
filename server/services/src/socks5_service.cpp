#include "socks5_service.h"

namespace yuan::server
{

    Socks5Service::Socks5Service(int port, yuan::net::socks5::Socks5ServerConfig config)
        : port_(port), config_(std::move(config)), server_(std::make_unique<yuan::net::socks5::Socks5Server>(config_)), host_({ "socks5", "socks5", port })
    {
    }

    Socks5Service::~Socks5Service()
    {
        stop();
    }

    bool Socks5Service::init()
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

    void Socks5Service::set_runtime_context(const yuan::app::RuntimeContext & context)
    {
        host_.set_runtime_context(context);
        shared_runtime_ = context.shared_runtime;
    }

    void Socks5Service::start()
    {
        host_.start([this]() { server_->serve(); });
    }

    void Socks5Service::stop()
    {
        host_.stop([this]() { server_->stop(); });
    }

    yuan::net::socks5::Socks5Server &Socks5Service::server()
    {
        return *server_;
    }

    const yuan::net::socks5::Socks5Server &Socks5Service::server() const
    {
        return *server_;
    }

    void Socks5Service::set_handler(yuan::net::socks5::Socks5Handler * handler)
    {
        handler_ = handler;
        if (server_) {
            server_->set_handler(handler);
        }
    }

} // namespace yuan::server
