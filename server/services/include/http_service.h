#ifndef __SERVER_HTTP_SERVICE_H__
#define __SERVER_HTTP_SERVICE_H__

#include "http_server.h"
#include "service.h"

#include <atomic>
#include <memory>
#include <thread>

namespace yuan::server
{

class HttpService : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
{
public:
    explicit HttpService(int port, yuan::net::http::HttpServerConfig config = {});
    ~HttpService() override;

    bool init() override;
    void start() override;
    void stop() override;
    void set_runtime_context(const yuan::app::RuntimeContext &context) override;

    yuan::net::http::HttpServer& server();
    const yuan::net::http::HttpServer& server() const;

private:
    int port_;
    yuan::net::http::HttpServerConfig config_;
    yuan::app::RuntimeContext runtime_context_{};
    std::unique_ptr<yuan::net::http::HttpServer> server_;
    std::atomic<bool> started_{false};
    std::thread worker_;
};

} // namespace yuan::server

#endif
