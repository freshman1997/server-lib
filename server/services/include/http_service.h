#ifndef __SERVER_HTTP_SERVICE_H__
#define __SERVER_HTTP_SERVICE_H__

#include "http_server.h"
#include "server_runtime_host.h"
#include "service.h"

#include <memory>

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

        yuan::net::http::HttpServer &server();
        const yuan::net::http::HttpServer &server() const;

    private:
        int port_;
        yuan::net::http::HttpServerConfig config_;
        std::unique_ptr<yuan::net::http::HttpServer> server_;
        ServerRuntimeHost host_;
        yuan::net::NetworkRuntime *shared_runtime_ = nullptr;
    };

} // namespace yuan::server

#endif
