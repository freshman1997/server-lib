#ifndef __YUAN_PLUGIN_HOST_SERVICE_REGISTRY_H__
#define __YUAN_PLUGIN_HOST_SERVICE_REGISTRY_H__

#include <any>
#include <memory>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace yuan::plugin
{

struct PluginContext;

struct PluginServiceDescriptor
{
    std::string name;
    std::string plugin_name;
    std::string type_name;
    std::string contract_id;
    int contract_version = 1;
    bool managed_lifecycle = false;
};

class PluginService
{
public:
    virtual ~PluginService() = default;

    virtual bool init(const PluginContext & /*context*/) { return true; }
    virtual void start() {}
    virtual void stop() {}
};

template <typename T>
std::any make_plugin_service(std::shared_ptr<T> service)
{
    static_assert(std::is_base_of_v<PluginService, T>, "T must derive from PluginService");
    return std::static_pointer_cast<PluginService>(std::move(service));
}

class HostServiceRegistry
{
public:
    virtual ~HostServiceRegistry() = default;

    virtual bool register_service(const std::string &plugin_name,
                                  const PluginServiceDescriptor &descriptor,
                                  std::any service) = 0;

    bool register_service(const std::string &plugin_name,
                          const std::string &name,
                          const std::string &type_name,
                          std::any service)
    {
        PluginServiceDescriptor descriptor;
        descriptor.name = name;
        descriptor.plugin_name = plugin_name;
        descriptor.type_name = type_name;
        descriptor.contract_id = name;
        descriptor.contract_version = 1;
        return register_service(plugin_name, descriptor, std::move(service));
    }

    virtual void unregister_plugin_services(const std::string &plugin_name) = 0;

    virtual std::any find_service(const std::string &name) const = 0;
    virtual bool describe_service(const std::string &name, PluginServiceDescriptor &descriptor) const = 0;

    virtual std::vector<PluginServiceDescriptor> list_services() const = 0;

    virtual bool has_service(const std::string &name) const = 0;

    template <typename T>
    bool register_managed_service(const std::string &plugin_name,
                                  const std::string &name,
                                  std::shared_ptr<T> service,
                                  const std::string &contract_id = {},
                                  int contract_version = 1)
    {
        static_assert(std::is_base_of_v<PluginService, T>, "T must derive from PluginService");

        PluginServiceDescriptor descriptor;
        descriptor.name = name;
        descriptor.plugin_name = plugin_name;
        descriptor.type_name = typeid(T).name();
        descriptor.contract_id = contract_id.empty() ? name : contract_id;
        descriptor.contract_version = contract_version;
        descriptor.managed_lifecycle = true;
        return register_service(plugin_name, descriptor, make_plugin_service(std::move(service)));
    }

    template <typename T>
    std::shared_ptr<T> find_service_as(const std::string &name) const
    {
        const auto service = find_service(name);
        if (!service.has_value()) {
            return {};
        }

        if (const auto exact = std::any_cast<std::shared_ptr<T>>(&service)) {
            return *exact;
        }

        if (const auto base = std::any_cast<std::shared_ptr<PluginService>>(&service)) {
            return std::dynamic_pointer_cast<T>(*base);
        }

        return {};
    }
};

} // namespace yuan::plugin

#endif
