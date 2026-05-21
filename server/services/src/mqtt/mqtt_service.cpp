#include "mqtt/mqtt_service.h"

namespace yuan::server
{
    MqttService::MqttService(int port, yuan::net::mqtt::MqttServerConfig config)
        : port_(port),
          config_(std::move(config)),
          server_(std::make_unique<yuan::net::mqtt::MqttServer>(config_)),
          enhanced_handler_(std::make_unique<MqttEnhancedHandler>()),
          host_({ "mqtt", "mqtt", port })
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
        } else if (use_enhanced_handler_ && enhanced_handler_) {
            server_->set_handler(enhanced_handler_.get());
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
        if (shared_runtime_) {
            host_.start_inline([this]() { server_->serve(); });
            return;
        }
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

    MqttEnhancedHandler & MqttService::enhanced_handler()
    {
        if (!enhanced_handler_) {
            enhanced_handler_ = std::make_unique<MqttEnhancedHandler>();
        }
        return *enhanced_handler_;
    }

    const MqttEnhancedHandler & MqttService::enhanced_handler() const
    {
        return *enhanced_handler_;
    }

    void MqttService::use_enhanced_handler(bool enabled)
    {
        use_enhanced_handler_ = enabled;
        if (!enabled)
            return;
        if (!enhanced_handler_) {
            enhanced_handler_ = std::make_unique<MqttEnhancedHandler>();
        }
        if (server_ && !handler_) {
            server_->set_handler(enhanced_handler_.get());
        }
    }

    bool MqttService::save_retained_store(const std::string & path) const
    {
        if (!server_) {
            return false;
        }
        return server_->save_retained_store(path);
    }

    bool MqttService::load_retained_store(const std::string & path)
    {
        if (!server_) {
            return false;
        }
        return server_->load_retained_store(path);
    }

    bool MqttService::save_session_store(const std::string & path) const
    {
        if (!server_) {
            return false;
        }
        return server_->save_session_store(path);
    }

    bool MqttService::load_session_store(const std::string & path)
    {
        if (!server_) {
            return false;
        }
        return server_->load_session_store(path);
    }

    bool MqttService::save_policy_store(const std::string & path) const
    {
        if (!enhanced_handler_) {
            return false;
        }
        return enhanced_handler_->save_policy_file(path);
    }

    bool MqttService::load_policy_store(const std::string & path)
    {
        if (!enhanced_handler_) {
            enhanced_handler_ = std::make_unique<MqttEnhancedHandler>();
        }
        return enhanced_handler_->load_policy_file(path);
    }
}
