#ifndef __YUAN_APP_PLUGIN_SERVICE_REGISTRY_ADAPTER_H__
#define __YUAN_APP_PLUGIN_SERVICE_REGISTRY_ADAPTER_H__

#include "plugin/host_service_registry.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::app
{

class PluginServiceRegistryAdapter : public plugin::HostServiceRegistry
{
public:
    PluginServiceRegistryAdapter() = default;
    ~PluginServiceRegistryAdapter() override = default;

    bool register_service(const std::string &plugin_name,
                          const plugin::PluginServiceDescriptor &descriptor,
                          std::any service) override;

    void unregister_plugin_services(const std::string &plugin_name) override;

    std::any find_service(const std::string &name) const override;
    bool describe_service(const std::string &name, plugin::PluginServiceDescriptor &descriptor) const override;

    std::vector<plugin::PluginServiceDescriptor> list_services() const override;

    bool has_service(const std::string &name) const override;

    bool init_plugin_services(const std::string &plugin_name, const plugin::PluginContext &context);
    void start_plugin_services(const std::string &plugin_name);
    void stop_plugin_services(const std::string &plugin_name);
    void stop_all_plugin_services();

private:
    struct ServiceEntry
    {
        plugin::PluginServiceDescriptor descriptor;
        std::any instance;
        std::shared_ptr<plugin::PluginService> managed_service;
        bool initialized = false;
        bool running = false;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ServiceEntry> services_;            // name -> entry
    std::unordered_map<std::string, std::vector<std::string>> plugin_services_; // plugin_name -> [service_names]
};

} // namespace yuan::app

#endif
