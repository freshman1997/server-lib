#ifndef __YUAN_APP_PLUGIN_PROTOCOL_SERVICE_ADAPTER_H__
#define __YUAN_APP_PLUGIN_PROTOCOL_SERVICE_ADAPTER_H__

#include "application.h"
#include "plugin/plugin_manifest.h"
#include "plugin/plugin_protocol_handler.h"
#include "service.h"
#include "service_registry.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace yuan::app
{
    class PluginHostService;
}

namespace yuan::net
{
    class AsyncListenerHost;
}

namespace yuan::app
{

    struct PluginProtocolActiveConnectionTracker;

    class PluginProtocolServiceAdapter final : public Service, public RuntimeContextAwareService
    {
    public:
        PluginProtocolServiceAdapter(std::string plugin_path,
                                     plugin::ProtocolServiceDescriptor protocol_service);
        ~PluginProtocolServiceAdapter() override;

        void set_runtime_context(const RuntimeContext &context) override;
        bool init() override;
        void start() override;
        void stop() override;

        const std::string &plugin_path() const noexcept;
        const plugin::ProtocolServiceDescriptor &protocol_service() const noexcept;
        bool initialized() const noexcept;
        bool started() const noexcept;

    private:
        bool start_protocol_listener();
        void stop_protocol_listener();

        std::string plugin_path_;
        plugin::ProtocolServiceDescriptor protocol_service_;
        RuntimeContext runtime_context_{};
        plugin::PluginProtocolHandlerRegistry handler_registry_;
        std::unique_ptr<PluginHostService> host_;
        std::unique_ptr<net::AsyncListenerHost> listener_;
        std::shared_ptr<PluginProtocolActiveConnectionTracker> connection_tracker_;
        bool initialized_ = false;
        bool started_ = false;
    };

    std::optional<ServiceDescriptor> make_plugin_protocol_service_descriptor(
        const plugin::ProtocolServiceDescriptor &protocol_service,
        ServicePlacement placement = {});

    std::vector<ServiceDescriptor> make_plugin_protocol_service_descriptors(
        const std::vector<plugin::ProtocolServiceDescriptor> &protocol_services,
        ServicePlacement placement = {});

    bool add_plugin_protocol_service(Application &application,
                                     std::string plugin_path,
                                     const plugin::ProtocolServiceDescriptor &protocol_service,
                                     ServicePlacement placement = {});

    std::size_t add_plugin_protocol_services(Application &application,
                                             std::string plugin_path,
                                             const std::vector<plugin::ProtocolServiceDescriptor> &protocol_services,
                                             ServicePlacement placement = {});

} // namespace yuan::app

#endif
