#include "rtsp_service.h"

namespace yuan::server
{

RtspService::RtspService(int port)
    : port_(port),
      server_(std::make_unique<yuan::net::rtsp::RtspServer>()),
      host_({ "rtsp", "rtsp", port })
{
}

RtspService::~RtspService()
{
    stop();
}

bool RtspService::init()
{
    if (!server_ || port_ <= 0) {
        return false;
    }
    if (shared_runtime_) {
        return server_->init(port_, *shared_runtime_);
    }
    return server_->init(port_);
}

void RtspService::set_runtime_context(const yuan::app::RuntimeContext &context)
{
    host_.set_runtime_context(context);
    shared_runtime_ = context.shared_runtime;
}

void RtspService::start()
{
    if (shared_runtime_) {
        host_.start_inline([this]() { server_->serve(); });
        return;
    }
    host_.start([this]() { server_->serve(); });
}

void RtspService::stop()
{
    host_.stop([this]() { server_->stop(); });
}

yuan::net::rtsp::RtspServer &RtspService::server()
{
    return *server_;
}

const yuan::net::rtsp::RtspServer &RtspService::server() const
{
    return *server_;
}

} // namespace yuan::server
