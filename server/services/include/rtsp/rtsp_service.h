#ifndef __SERVER_RTSP_SERVICE_H__
#define __SERVER_RTSP_SERVICE_H__

#include "rtsp_server.h"
#include "server/proxy/include/server_runtime_host.h"
#include "service.h"

#include <memory>

namespace yuan::server
{

class RtspService : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
{
public:
    explicit RtspService(int port);
    ~RtspService() override;

    bool init() override;
    void start() override;
    void stop() override;
    void set_runtime_context(const yuan::app::RuntimeContext &context) override;

    yuan::net::rtsp::RtspServer &server();
    const yuan::net::rtsp::RtspServer &server() const;

private:
    int port_;
    std::unique_ptr<yuan::net::rtsp::RtspServer> server_;
    ServerRuntimeHost host_;
    yuan::net::NetworkRuntime *shared_runtime_ = nullptr;
};

} // namespace yuan::server

#endif
