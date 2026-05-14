#ifndef __SERVER_MQTT_SERVICE_H__
#define __SERVER_MQTT_SERVICE_H__

#include "mqtt.h"
#include "mqtt_service_enhanced_handler.h"
#include "server_runtime_host.h"
#include "service.h"

#include <memory>
#include <string>

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

        MqttEnhancedHandler &enhanced_handler();
        const MqttEnhancedHandler &enhanced_handler() const;
        void use_enhanced_handler(bool enabled = true);
        bool save_retained_store(const std::string &path) const;
        bool load_retained_store(const std::string &path);
        bool save_session_store(const std::string &path) const;
        bool load_session_store(const std::string &path);
        bool save_policy_store(const std::string &path) const;
        bool load_policy_store(const std::string &path);

    private:
        int port_;
        yuan::net::mqtt::MqttServerConfig config_;
        std::unique_ptr<yuan::net::mqtt::MqttServer> server_;
        yuan::net::mqtt::MqttHandler *handler_ = nullptr;
        std::unique_ptr<MqttEnhancedHandler> enhanced_handler_;
        bool use_enhanced_handler_ = true;
        ServerRuntimeHost host_;
        yuan::net::NetworkRuntime *shared_runtime_ = nullptr;
    };
}

#endif
