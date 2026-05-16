#include "ftp_service.h"

namespace yuan::server
{

    FtpService::FtpService(int port)
        : port_(port), server_(std::make_unique<yuan::net::ftp::FtpServer>()), host_({ "ftp", "ftp", port })
    {
    }

    FtpService::~FtpService()
    {
        stop();
    }

    bool FtpService::init()
    {
        return port_ > 0 && server_ != nullptr;
    }

    void FtpService::set_runtime_context(const yuan::app::RuntimeContext & context)
    {
        host_.set_runtime_context(context);
        shared_runtime_ = context.shared_runtime;
    }

    void FtpService::start()
    {
        if (shared_runtime_) {
            host_.start_inline([this]() { server_->serve(port_, *shared_runtime_); });
        } else {
            host_.start([this]() { server_->serve(port_); });
        }
    }

    void FtpService::stop()
    {
        host_.stop([this]() { server_->quit(); });
    }

    yuan::net::ftp::FtpServer &FtpService::server()
    {
        return *server_;
    }

    const yuan::net::ftp::FtpServer &FtpService::server() const
    {
        return *server_;
    }

} // namespace yuan::server
