#include "plugin_protocol_service_adapter.h"

#include "logger.h"
#include "plugin_host_service.h"

#include <utility>

namespace yuan::app
{
    namespace
    {
        bool is_valid_protocol_service(const plugin::ProtocolServiceDescriptor &protocol_service)
        {
            return !protocol_service.plugin_id.empty() &&
                   !protocol_service.name.empty() &&
                   !protocol_service.contract_id.empty() &&
                   protocol_service.port >= 0;
        }
    }

    PluginProtocolServiceAdapter::PluginProtocolServiceAdapter(
        std::string plugin_path,
        plugin::ProtocolServiceDescriptor protocol_service)
        : plugin_path_(std::move(plugin_path)),
          protocol_service_(std::move(protocol_service))
    {
    }

    PluginProtocolServiceAdapter::~PluginProtocolServiceAdapter()
    {
        stop();
    }

    void PluginProtocolServiceAdapter::set_runtime_context(const RuntimeContext &context)
    {
        runtime_context_ = context;
        if (host_) {
            host_->set_runtime_context(runtime_context_);
        }
    }

    bool PluginProtocolServiceAdapter::init()
    {
        if (initialized_) {
            return true;
        }
        if (plugin_path_.empty() || !is_valid_protocol_service(protocol_service_)) {
            return false;
        }

        host_ = std::make_unique<PluginHostService>(
            plugin_path_,
            std::vector<std::string>{ protocol_service_.plugin_id });
        host_->set_runtime_context(runtime_context_);

        if (!host_->init()) {
            LOG_ERROR(
                "plugin protocol service '{}.{}' failed to initialize plugin runtime",
                protocol_service_.plugin_id,
                protocol_service_.name);
            host_.reset();
            return false;
        }

        initialized_ = true;
        return true;
    }

    void PluginProtocolServiceAdapter::start()
    {
        if (started_) {
            return;
        }
        if (!initialized_ && !init()) {
            return;
        }
        if (host_) {
            host_->start();
        }
        started_ = true;
    }

    void PluginProtocolServiceAdapter::stop()
    {
        if (host_) {
            host_->stop();
            host_.reset();
        }
        started_ = false;
        initialized_ = false;
    }

    const std::string &PluginProtocolServiceAdapter::plugin_path() const noexcept
    {
        return plugin_path_;
    }

    const plugin::ProtocolServiceDescriptor &PluginProtocolServiceAdapter::protocol_service() const noexcept
    {
        return protocol_service_;
    }

    bool PluginProtocolServiceAdapter::initialized() const noexcept
    {
        return initialized_;
    }

    bool PluginProtocolServiceAdapter::started() const noexcept
    {
        return started_;
    }

    std::optional<ServiceDescriptor> make_plugin_protocol_service_descriptor(
        const plugin::ProtocolServiceDescriptor &protocol_service,
        ServicePlacement placement)
    {
        if (!is_valid_protocol_service(protocol_service)) {
            return std::nullopt;
        }

        ServiceDescriptor descriptor;
        descriptor.name = protocol_service.plugin_id + "." + protocol_service.name;
        descriptor.type_name = protocol_service.type.empty()
            ? "plugin.protocol"
            : "plugin.protocol." + protocol_service.type;
        descriptor.contract_id = protocol_service.contract_id;
        descriptor.contract_version = protocol_service.contract_version;
        descriptor.placement = placement;
        descriptor.endpoints.push_back(ServiceEndpoint{
            protocol_service.name,
            protocol_service.host.empty() ? "0.0.0.0" : protocol_service.host,
            protocol_service.port,
            protocol_service.protocol.empty() ? "tcp" : protocol_service.protocol
        });
        return descriptor;
    }

    std::vector<ServiceDescriptor> make_plugin_protocol_service_descriptors(
        const std::vector<plugin::ProtocolServiceDescriptor> &protocol_services,
        ServicePlacement placement)
    {
        std::vector<ServiceDescriptor> descriptors;
        descriptors.reserve(protocol_services.size());
        for (const auto &protocol_service : protocol_services) {
            auto descriptor = make_plugin_protocol_service_descriptor(protocol_service, placement);
            if (descriptor) {
                descriptors.push_back(std::move(*descriptor));
            }
        }
        return descriptors;
    }

    bool add_plugin_protocol_service(
        Application &application,
        std::string plugin_path,
        const plugin::ProtocolServiceDescriptor &protocol_service,
        ServicePlacement placement)
    {
        auto descriptor = make_plugin_protocol_service_descriptor(protocol_service, placement);
        if (!descriptor || plugin_path.empty()) {
            return false;
        }

        auto captured_path = std::move(plugin_path);
        auto captured_service = protocol_service;
        return application.add_service(
            std::move(*descriptor),
            [captured_path = std::move(captured_path), captured_service]() {
                return std::make_shared<PluginProtocolServiceAdapter>(
                    captured_path,
                    captured_service);
            });
    }

    std::size_t add_plugin_protocol_services(
        Application &application,
        std::string plugin_path,
        const std::vector<plugin::ProtocolServiceDescriptor> &protocol_services,
        ServicePlacement placement)
    {
        std::size_t added = 0;
        for (const auto &protocol_service : protocol_services) {
            if (add_plugin_protocol_service(application, plugin_path, protocol_service, placement)) {
                ++added;
            }
        }
        return added;
    }

} // namespace yuan::app
