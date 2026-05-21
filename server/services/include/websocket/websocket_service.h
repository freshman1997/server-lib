#ifndef __SERVER_WEBSOCKET_SERVICE_H__
#define __SERVER_WEBSOCKET_SERVICE_H__

#include "server/proxy/include/server_runtime_host.h"
#include "service.h"
#include "websocket.h"

#include <memory>

namespace yuan::server
{

    class WebSocketService : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        explicit WebSocketService(int port);
        ~WebSocketService() override;

        bool init() override;
        void start() override;
        void stop() override;
        void set_runtime_context(const yuan::app::RuntimeContext &context) override;

        yuan::net::websocket::WebSocketServer &server();
        const yuan::net::websocket::WebSocketServer &server() const;

        void set_data_handler(yuan::net::websocket::WebSocketDataHandler *handler);

    private:
        int port_;
        std::unique_ptr<yuan::net::websocket::WebSocketServer> server_;
        yuan::net::websocket::WebSocketDataHandler *handler_ = nullptr;
        ServerRuntimeHost host_;
        yuan::net::NetworkRuntime *shared_runtime_ = nullptr;
    };

} // namespace yuan::server

#endif
