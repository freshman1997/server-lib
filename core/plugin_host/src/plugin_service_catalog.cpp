#include "plugin_service_catalog.h"

#include "service.h"
#include "service_registry.h"

#include <typeinfo>
#include <utility>

namespace yuan::app
{

PluginServiceCatalog::PluginServiceCatalog(std::shared_ptr<ServiceRegistry> registry)
    : registry_(std::move(registry))
{
}

bool PluginServiceCatalog::has_service(const std::string &name) const
{
    return registry_ && registry_->find_service(name);
}

std::vector<plugin::HostServiceDescriptor> PluginServiceCatalog::list_services() const
{
    std::vector<plugin::HostServiceDescriptor> descriptors;
    if (!registry_) {
        return descriptors;
    }

    for (const auto &service : registry_->list_descriptors()) {
        plugin::HostServiceDescriptor descriptor;
        descriptor.name = service.name;
        descriptor.type_name = service.type_name;
        descriptor.contract_id = service.contract_id;
        descriptor.contract_version = service.contract_version;
        descriptors.push_back(std::move(descriptor));
    }

    return descriptors;
}

std::any PluginServiceCatalog::get_service(const std::string &name) const
{
    if (!registry_) {
        return {};
    }
    auto typed = registry_->find_typed_service(name);
    if (typed.has_value()) {
        return typed;
    }
    auto svc = registry_->find_service(name);
    if (!svc) {
        return {};
    }
    return svc;
}

bool PluginServiceCatalog::describe_service(const std::string &name,
                                            plugin::HostServiceDescriptor &descriptor) const
{
    if (!registry_) {
        return false;
    }

    yuan::app::ServiceDescriptor app_descriptor;
    if (!registry_->describe_service(name, app_descriptor)) {
        return false;
    }

    descriptor.name = app_descriptor.name;
    descriptor.type_name = app_descriptor.type_name;
    descriptor.contract_id = app_descriptor.contract_id;
    descriptor.contract_version = app_descriptor.contract_version;
    return true;
}

} // namespace yuan::app
