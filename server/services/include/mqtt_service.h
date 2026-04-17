#ifndef __SERVER_MQTT_SERVICE_H__
#define __SERVER_MQTT_SERVICE_H__

#include "mqtt.h"
#include "server_runtime_host.h"
#include "service.h"

#include <memory>

namespace yuan::server
{
    class MqttService : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        explicit MqttService(int port, yuan::net::mqtt::MqttServerConfig config = {});
        ~MqttService() override;

        bool init() override;
        void start() override;
        void stop() override;
        void set_runtime_context(const yuan::app::RuntimeContext &context) override;

        yuan::net::mqtt::MqttServer &server();
        const yuan::net::mqtt::MqttServer &server() const;

        void set_handler(yuan::net::mqtt::MqttHandler *handler);

    private:
        int port_;
        yuan::net::mqtt::MqttServerConfig config_;
        std::unique_ptr<yuan::net::mqtt::MqttServer> server_;
        yuan::net::mqtt::MqttHandler *handler_ = nullptr;
        ServerRuntimeHost host_;
        yuan::net::NetworkRuntime *shared_runtime_ = nullptr;
    };
}

#endif
