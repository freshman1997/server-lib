#include "mqtt_service.h"

namespace yuan::server
{
    MqttService::MqttService(int port, yuan::net::mqtt::MqttServerConfig config)
        : port_(port), config_(std::move(config)), server_(std::make_unique<yuan::net::mqtt::MqttServer>(config_)), host_({ "mqtt", "mqtt", port })
    {
    }

    MqttService::~MqttService()
    {
        stop();
    }

    bool MqttService::init()
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

    void MqttService::set_runtime_context(const yuan::app::RuntimeContext & context)
    {
        host_.set_runtime_context(context);
        shared_runtime_ = context.shared_runtime;
    }

    void MqttService::start()
    {
        host_.start([this]() { server_->serve(); });
    }

    void MqttService::stop()
    {
        host_.stop([this]() { server_->stop(); });
    }

    yuan::net::mqtt::MqttServer &MqttService::server()
    {
        return *server_;
    }

    const yuan::net::mqtt::MqttServer &MqttService::server() const
    {
        return *server_;
    }

    void MqttService::set_handler(yuan::net::mqtt::MqttHandler * handler)
    {
        handler_ = handler;
        if (server_) {
            server_->set_handler(handler);
        }
    }
}
